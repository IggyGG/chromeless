// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// RED tests for the browser-process PCF seam — see cloud_browser_pcf.h.
//
// These tests are RED-by-construction pre-T17: the build env is a
// from-source Chromium / libwebrtc tree which is exercised by the M0
// chromeless-build K8s Job on triform-2 (operator-dispatched). Each
// fixture documents the assertion shape that will go GREEN once the
// build env runs and the seam compiles + links.
//
// Test surface (CV2-26 + CV2-27 TDD plan):
//   * CloudBrowserPcfTest.DepsCarryInjectedEncoderFactory
//   * CloudBrowserPcfTest.DepsAudioDeviceModuleSlotHonoursInjection
//   * CloudBrowserPcfTest.PcfVideoSenderCapsMatchFactoryFormats
//   * CloudBrowserPcfLogTest.FormatsDeterministicCodecLine

#include "capture/build-integration/cloud_browser_pcf.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_parameters.h"
#include "api/scoped_refptr.h"
#include "capture/encoder/encoder_factory.h"
#include "modules/audio_device/include/test_audio_device.h"
#include "rtc_base/thread.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace cloud_browser {
namespace {

using ::testing::IsSupersetOf;
using ::testing::UnorderedElementsAre;

// Owns 3 dedicated rtc::Threads matching the embedder shape — network,
// worker, signaling — Start()ed in the ctor and Stop()ped in the dtor.
// Mirrors the per-test lifetime CloudBrowserBrowserMainParts holds for
// real.
class ScopedPcfThreads {
 public:
  ScopedPcfThreads()
      : network_(rtc::Thread::CreateWithSocketServer()),
        worker_(rtc::Thread::Create()),
        signaling_(rtc::Thread::Create()) {
    network_->SetName("cb-test-net", nullptr);
    worker_->SetName("cb-test-worker", nullptr);
    signaling_->SetName("cb-test-signaling", nullptr);
    network_->Start();
    worker_->Start();
    signaling_->Start();
  }

  ~ScopedPcfThreads() {
    signaling_->Stop();
    worker_->Stop();
    network_->Stop();
  }

  rtc::Thread* network() { return network_.get(); }
  rtc::Thread* worker() { return worker_.get(); }
  rtc::Thread* signaling() { return signaling_.get(); }

 private:
  std::unique_ptr<rtc::Thread> network_;
  std::unique_ptr<rtc::Thread> worker_;
  std::unique_ptr<rtc::Thread> signaling_;
};

// Extract uppercase codec names from a RtpCapabilities.codecs vector,
// preserving order. Helper for the codec-set test.
std::vector<std::string> CodecNames(
    const std::vector<webrtc::RtpCodecCapability>& codecs) {
  std::vector<std::string> names;
  names.reserve(codecs.size());
  for (const auto& c : codecs) {
    names.push_back(c.name);
  }
  return names;
}

// --- BuildCloudBrowserPcfDependencies ------------------------------------

TEST(CloudBrowserPcfTest, DepsCarryInjectedEncoderFactory) {
  ScopedPcfThreads threads;
  webrtc::Environment env = webrtc::CreateEnvironment();

  rtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      CreateCloudBrowserDefaultAudioDeviceModule();

  webrtc::PeerConnectionFactoryDependencies deps =
      BuildCloudBrowserPcfDependencies(threads.network(), threads.worker(),
                                       threads.signaling(), env, adm);

  // Thread slots: identity comparison — the deps must point at the
  // exact threads passed in (no implicit wrapping or substitution).
  EXPECT_EQ(deps.network_thread, threads.network());
  EXPECT_EQ(deps.worker_thread, threads.worker());
  EXPECT_EQ(deps.signaling_thread, threads.signaling());

  // Encoder/decoder factories must be populated. video_encoder_factory
  // must specifically be a CloudBrowserVideoEncoderFactory; the
  // dynamic_cast is the assertion of identity for the injected factory.
  ASSERT_NE(deps.video_encoder_factory, nullptr);
  EXPECT_NE(dynamic_cast<CloudBrowserVideoEncoderFactory*>(
                deps.video_encoder_factory.get()),
            nullptr);
  EXPECT_NE(deps.video_decoder_factory, nullptr);
  EXPECT_NE(deps.audio_encoder_factory, nullptr);
  EXPECT_NE(deps.audio_decoder_factory, nullptr);
}

TEST(CloudBrowserPcfTest, DepsAudioDeviceModuleSlotHonoursInjection) {
  ScopedPcfThreads threads;
  webrtc::Environment env = webrtc::CreateEnvironment();

  // Path A — caller injects an ADM. The deps audio_device_module slot
  // must hold exactly that ADM (identity, not "any non-null ADM").
  // This is the M5.5 cross-module contract: M5.5 passes its real ADM
  // here and expects it to survive PCF construction unchanged.
  rtc::scoped_refptr<webrtc::AudioDeviceModule> injected =
      webrtc::TestAudioDeviceModule::CreateTestAudioDeviceModule(
          webrtc::TestAudioDeviceModule::CreatePulsedNoiseCapturer(
              /*max_amplitude=*/0, /*sampling_frequency_in_hz=*/48000),
          /*renderer=*/nullptr);
  ASSERT_NE(injected, nullptr);

  webrtc::PeerConnectionFactoryDependencies deps_injected =
      BuildCloudBrowserPcfDependencies(threads.network(), threads.worker(),
                                       threads.signaling(), env, injected);
  EXPECT_EQ(deps_injected.adm.get(), injected.get());

  // Path B — caller passes null. The default path must substitute the
  // M1 dummy / no-audio ADM via CreateCloudBrowserDefaultAudioDevice
  // Module(). Non-null assertion is the contract (the specific
  // identity of the static dummy is an implementation detail).
  webrtc::PeerConnectionFactoryDependencies deps_default =
      BuildCloudBrowserPcfDependencies(threads.network(), threads.worker(),
                                       threads.signaling(), env, nullptr);
  EXPECT_NE(deps_default.adm.get(), nullptr);
}

// --- CreateCloudBrowserPcf -----------------------------------------------

TEST(CloudBrowserPcfTest, PcfVideoSenderCapsMatchFactoryFormats) {
  ScopedPcfThreads threads;
  webrtc::Environment env = webrtc::CreateEnvironment();
  rtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      CreateCloudBrowserDefaultAudioDeviceModule();

  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pcf =
      CreateCloudBrowserPcf(threads.network(), threads.worker(),
                            threads.signaling(), env, adm);
  ASSERT_NE(pcf, nullptr);

  webrtc::RtpCapabilities caps =
      pcf->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
  std::vector<std::string> names = CodecNames(caps.codecs);

  // M1 Finding-A-ratified expected set under default Config{}:
  // VP9 + H264 + AV1 — VP8 absent.
  //
  // Use IsSupersetOf for resilience to libwebrtc adding RTX/red/ulpfec
  // entries to the capability list (those are mandatory companions and
  // appear alongside the primary codecs). The codec-set contract is
  // about what's PRESENT, not about strict equality.
  EXPECT_THAT(names, IsSupersetOf({"VP9", "H264", "AV1"}));

  // VP8 must be absent — Config{}.enable_vp8 == false ⇒
  // GetSupportedFormats() omits VP8 ⇒ the sender capability list must
  // omit it too.
  for (const auto& name : names) {
    EXPECT_NE(name, "VP8");
  }
}

// --- FormatPcfVideoCodecLogLine ------------------------------------------

// Helper to build an RtpCodecCapability with just the name set —
// FormatPcfVideoCodecLogLine reads only .name.
webrtc::RtpCodecCapability MakeCodec(const std::string& name) {
  webrtc::RtpCodecCapability c;
  c.name = name;
  return c;
}

TEST(CloudBrowserPcfLogTest, FormatsDeterministicCodecLine) {
  // Case 1: M1-ratified expected set — VP9 + H264 + AV1 in factory
  // preference order.
  //
  // Provenance: synthetic pre-T17; replace with real cb-chromium
  // boot-log line after first green. The format below is pinned to
  // the M0 R5 scrape regex — drift silently breaks assertion #3.
  {
    std::vector<webrtc::RtpCodecCapability> codecs = {
        MakeCodec("VP9"), MakeCodec("H264"), MakeCodec("AV1")};
    EXPECT_EQ(FormatPcfVideoCodecLogLine(codecs),
              "CloudBrowser: PCF video sender codecs = [VP9,H264,AV1]");
  }

  // Case 2: single-element vector — VP8 alone. Confirms bracket-list
  // formatting works without separators.
  {
    std::vector<webrtc::RtpCodecCapability> codecs = {MakeCodec("VP8")};
    EXPECT_EQ(FormatPcfVideoCodecLogLine(codecs),
              "CloudBrowser: PCF video sender codecs = [VP8]");
  }

  // Case 3: empty vector — must render as the literal "[]" suffix,
  // not omit the brackets. The M0 R5 scrape regex matches on the
  // bracket pair; missing brackets would silently mis-classify the
  // "no codecs available" case as a regex non-match.
  {
    std::vector<webrtc::RtpCodecCapability> codecs;
    EXPECT_EQ(FormatPcfVideoCodecLogLine(codecs),
              "CloudBrowser: PCF video sender codecs = []");
  }

  // Case 4: stable order — confirm the fn does NOT sort. A reversed
  // input must render reversed. The factory preference order is the
  // observable contract here; sorting would silently change SDP
  // codec-preference ordering.
  {
    std::vector<webrtc::RtpCodecCapability> codecs = {
        MakeCodec("AV1"), MakeCodec("H264"), MakeCodec("VP9")};
    EXPECT_EQ(FormatPcfVideoCodecLogLine(codecs),
              "CloudBrowser: PCF video sender codecs = [AV1,H264,VP9]");
  }
}

}  // namespace
}  // namespace cloud_browser
