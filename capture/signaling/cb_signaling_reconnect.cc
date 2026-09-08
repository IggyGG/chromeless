// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Implementation of the M3 R7 reconnect + idle-timeout supervisor. See
// header for the design + scope + the "no WS-level pings" rationale;
// this file is mechanism.

#include "capture/signaling/cb_signaling_reconnect.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"

namespace cloud_browser::signaling {

namespace {

// RFC 6455 normal-closure code. Mirrored from R2's Disconnect()
// contract — when the inner client closes with this code we treat it
// as terminal (consumer-driven or a graceful remote shutdown) and do
// NOT reconnect.
constexpr uint16_t kCloseCodeNormal = 1000;

// Synthetic code we use when force-closing on idle timeout. Outside
// the RFC 6455 application range (4000-4999) so it doesn't clash with
// physics-side close codes; logged but never sent on the wire (the
// idle-close path doesn't reach the inner client's Disconnect()
// frame — see OnIdleFired).
//
// TODO(M3-R7-idle-close-code): once physics defines its own
// application-range close codes (see CV2-56 / M3 R6 draft), align
// this constant so the wrapper's "why did we reconnect" diagnostic
// uses the same vocabulary as the broker.
constexpr uint16_t kCloseCodeIdleTimeout = 4090;

// TODO(M3-R7-state-tostring): the State enum is private to the class
// so a free-function ToString would need friend access. For now logs
// emit the integer value; a future polish pass can move the enum to
// a namespace-scoped struct and add a real ToString helper.

}  // namespace

CbSignalingReconnect::CbSignalingReconnect(
    network::mojom::NetworkContext* network_context,
    WsClientConfig config,
    ReconnectConfig reconnect_config,
    ReconnectingClientObserver* observer)
    : network_context_(network_context),
      config_(std::move(config)),
      reconnect_config_(reconnect_config),
      observer_(observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(network_context_);
  DCHECK(observer_);
  // initial_backoff is the seed for the schedule; we keep it as the
  // "next" delay so the first reconnect waits initial_backoff.
  current_backoff_ = reconnect_config_.initial_backoff;
}

CbSignalingReconnect::~CbSignalingReconnect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Drop the observer pointer first so any late callback from
  // inner_'s destruction path is a no-op — R2's dtor "silently aborts
  // the dial without firing observer callbacks" per its contract, but
  // belt-and-braces.
  observer_ = nullptr;
  TearDown();
}

void CbSignalingReconnect::Connect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kIdle) {
    DVLOG(1) << "CbSignalingReconnect::Connect ignored, state=" << static_cast<int>(state_);
    return;
  }
  state_ = State::kConnecting;
  attempts_made_ = 0;
  current_backoff_ = reconnect_config_.initial_backoff;
  StartInnerDial();
}

bool CbSignalingReconnect::Send(const Envelope& envelope) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kOpen || !inner_) {
    DVLOG(2) << "CbSignalingReconnect::Send dropped, not connected";
    return false;
  }
  return inner_->Send(envelope);
}

void CbSignalingReconnect::Disconnect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::kClosed || state_ == State::kGaveUp) {
    return;
  }
  // CV2-REDIAL: mark this close as OURS before anything can report it back.
  // OnClosed treats every close it did not ask for as a broker-side drop to
  // redial, so without this latch a consumer-driven Disconnect would be
  // answered with a reconnect.
  disconnect_requested_ = true;

  // Cancel pending reconnect / idle timers BEFORE telling the inner
  // client to close — otherwise an inner OnClosed could race with a
  // backoff fire and we'd re-dial after the consumer asked us to
  // shut down.
  backoff_timer_.Stop();
  idle_timer_.Stop();

  if (inner_ && (inner_->is_connected() || inner_->is_closing())) {
    inner_->Disconnect();
    // The inner client's OnClosed will arrive shortly with code 1000;
    // our OnClosed override checks state_ and forwards to the
    // consumer's observer.
  } else {
    // No live channel — fire OnClosed synthetically so the consumer
    // sees a terminal event.
    state_ = State::kClosed;
    if (observer_) {
      observer_->OnClosed(kCloseCodeNormal, "wrapper-disconnect-noop");
    }
  }
}

bool CbSignalingReconnect::is_connected() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return state_ == State::kOpen;
}

bool CbSignalingReconnect::is_reconnecting() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return state_ == State::kBackingOff || state_ == State::kReconnecting;
}

uint32_t CbSignalingReconnect::attempts_made() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return attempts_made_;
}

// ---------------------------------------------------------------------
// SignalingClientObserver — the inner client calls these on us. We
// translate inner-client lifecycle into the consumer-facing
// ReconnectingClientObserver vocabulary.
// ---------------------------------------------------------------------

void CbSignalingReconnect::OnConnected() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  state_ = State::kOpen;
  // Successful (re)connect — reset the schedule so the next death
  // starts from initial_backoff again.
  attempts_made_ = 0;
  current_backoff_ = reconnect_config_.initial_backoff;
  ArmIdleTimer();
  if (observer_) {
    observer_->OnConnected();
  }
}

void CbSignalingReconnect::OnEnvelope(const Envelope& envelope) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Re-arm the idle timer on every inbound envelope — proof of life.
  ArmIdleTimer();
  if (observer_) {
    observer_->OnEnvelope(envelope);
  }
}

void CbSignalingReconnect::OnClosed(uint16_t code, std::string_view reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  idle_timer_.Stop();

  // Consumer-driven close OR graceful remote close → terminal.
  // CV2-REDIAL: terminal ONLY when we asked for it. A remote close — any
  // code, including a tidy 1000 from a broker shutting down for a rollout —
  // is a dead channel to redial. See disconnect_requested_.
  if (state_ == State::kClosed || disconnect_requested_) {
    state_ = State::kClosed;
    if (observer_) {
      observer_->OnClosed(code, reason);
    }
    return;
  }

  // Any other close → recover.
  HandleChannelDead(reason);
}

void CbSignalingReconnect::OnError(std::string_view reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  idle_timer_.Stop();
  // Errors mean the channel is half-broken per R2's contract — treat
  // identically to a non-clean close.
  HandleChannelDead(reason);
}

// ---------------------------------------------------------------------
// Internal mechanism.
// ---------------------------------------------------------------------

void CbSignalingReconnect::StartInnerDial() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Always construct a fresh inner — R2's State machine is single-
  // shot (see header design note above the State enum).
  inner_ = std::make_unique<SignalingWsClient>(
      network_context_, config_, /*observer=*/this);
  inner_->Connect();
}

base::TimeDelta CbSignalingReconnect::NextBackoff() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // The first call returns current_backoff_ unchanged
  // (= initial_backoff). Subsequent calls multiply, capped.
  base::TimeDelta next = current_backoff_;
  if (next > reconnect_config_.max_backoff) {
    next = reconnect_config_.max_backoff;
  }
  return next;
}

void CbSignalingReconnect::HandleChannelDead(std::string_view why) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Drop the dead inner client now — the next attempt builds fresh.
  // We don't call inner_->Disconnect() here; the inner is already in
  // its terminal kClosed (it just told us so via OnClosed) or kError-
  // equivalent state. Just release.
  inner_.reset();

  if (reconnect_config_.max_attempts == 0 ||
      attempts_made_ >= reconnect_config_.max_attempts) {
    state_ = State::kGaveUp;
    LOG(WARNING) << "CbSignalingReconnect: gave up after "
                 << attempts_made_ << " attempts, last reason=" << why;
    if (observer_) {
      observer_->OnGaveUp(attempts_made_);
    }
    return;
  }

  state_ = State::kBackingOff;
  const base::TimeDelta delay = NextBackoff();
  LOG(INFO) << "CbSignalingReconnect: reconnect in " << delay
            << ", attempt " << (attempts_made_ + 1) << "/"
            << reconnect_config_.max_attempts << ", reason=" << why;

  // Advance the schedule for the *next* backoff calculation.
  current_backoff_ = std::min(
      current_backoff_ * reconnect_config_.multiplier,
      reconnect_config_.max_backoff);

  backoff_timer_.Start(
      FROM_HERE, delay,
      base::BindOnce(&CbSignalingReconnect::OnBackoffFired,
                     weak_factory_.GetWeakPtr()));
}

void CbSignalingReconnect::OnBackoffFired() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kBackingOff) {
    // Disconnect() raced with the timer; honour the shutdown.
    return;
  }
  ++attempts_made_;
  state_ = State::kReconnecting;
  StartInnerDial();
}

void CbSignalingReconnect::OnIdleFired() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kOpen) {
    return;
  }
  LOG(INFO) << "CbSignalingReconnect: idle timeout fired after "
            << reconnect_config_.idle_timeout
            << ", forcing reconnect (synthetic code "
            << kCloseCodeIdleTimeout << ")";

  // We do NOT route through inner_->Disconnect() here. R2's
  // Disconnect() would send a 1000 close frame and our OnClosed
  // would then have to disambiguate "consumer asked to close" from
  // "we asked to close because idle". Simpler to skip the polite
  // close frame and rely on R2's "Destruction at any state is safe;
  // a mid-handshake destruction silently aborts the dial without
  // firing observer callbacks" contract: HandleChannelDead() drops
  // inner_ which closes the mojo receivers; the network service
  // sees the channel go away and reaps its half. Slightly less
  // polite than a proper 1000 close on the wire, but the remote is
  // by hypothesis silent / dead so the politeness is wasted.
  //
  // TODO(M3-R7-polite-idle-close): once we have telemetry from a
  // real broker rollout, revisit whether a polite close is worth
  // the extra round-trip latency on the reconnect path. Today's
  // assumption is "the remote is dead, just rebuild".
  HandleChannelDead("idle-timeout");
}

void CbSignalingReconnect::ArmIdleTimer() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (reconnect_config_.idle_timeout.is_zero()) {
    return;  // Idle-timeout disabled by config.
  }
  // OneShotTimer::Start cancels any pending fire and re-arms — exactly
  // the "reset on any inbound" semantics we want.
  idle_timer_.Start(
      FROM_HERE, reconnect_config_.idle_timeout,
      base::BindOnce(&CbSignalingReconnect::OnIdleFired,
                     weak_factory_.GetWeakPtr()));
}

void CbSignalingReconnect::TearDown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  backoff_timer_.Stop();
  idle_timer_.Stop();
  inner_.reset();
  state_ = State::kClosed;
}

}  // namespace cloud_browser::signaling
