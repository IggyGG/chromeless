// Webcam + microphone passthrough — client side (T81).
//
// Per docs/protocols/webcam-mic-passthrough.md, Path A: the local
// user grants camera/mic access via navigator.mediaDevices.
// getUserMedia(); we add the resulting MediaStreamTracks to the
// existing RTCPeerConnection. The streamer page's ontrack handler
// then routes the inbound media into a v4l2loopback / PulseAudio
// loopback inside the cloud Chromium pod.
//
// The client-side responsibilities:
//   1. Surface a "Share camera/mic" affordance + a "Stop sharing"
//      toggle. UI lives in main.ts; this module is the controller.
//   2. Call getUserMedia() on enable; reject cleanly on permission
//      denial / no device.
//   3. addTrack() the resulting tracks to the supplied
//      RTCPeerConnection. The browser fires negotiationneeded; the
//      caller is expected to drive the renegotiation per the
//      existing T37 protocol — we expose an onNeedRenegotiate
//      callback so main.ts can wire that into its existing
//      sendRequestRenegotiate flow.
//   4. On disable: stop() each track, removeTrack from the pc, and
//      trigger another renegotiation so the m= sections drop.
//
// Strict v1 stance per the threat model:
//   - Never auto-enable from URL params or storage.
//   - Always require an explicit user-gesture call to enable().
//   - Always tear down completely on disable() — no half-states.

export interface PassthroughOptions {
  /** Constrain to a specific camera (deviceId). Optional; default = UA picks. */
  videoDeviceId?: string;
  /** Constrain to a specific mic (deviceId). Optional; default = UA picks. */
  audioDeviceId?: string;
  /** Disable video — mic-only passthrough. Default false. */
  disableVideo?: boolean;
  /** Disable audio — camera-only passthrough. Default false. */
  disableAudio?: boolean;
  /** Override navigator.mediaDevices for tests. */
  mediaDevices?: { getUserMedia(c: MediaStreamConstraints): Promise<MediaStream> };
  /** Called whenever the set of active tracks changes. UI hook. */
  onStateChange?: (state: PassthroughState) => void;
  /** Called when a renegotiation is required. Caller invokes signaling. */
  onNeedRenegotiate?: (reason: "track_added" | "track_removed") => void;
  /** Called on enable() failure — permission denied, no device, etc. */
  onError?: (err: PassthroughError) => void;
}

export interface PassthroughState {
  enabled: boolean;
  videoTrackActive: boolean;
  audioTrackActive: boolean;
  /** Non-null only when enabled — the live MediaStream attached to the PC. */
  stream: MediaStream | null;
}

export class PassthroughError extends Error {
  constructor(public readonly code: string, message: string) {
    super(message);
    this.name = "PassthroughError";
  }
}

// Surface of an RTCPeerConnection that we actually depend on. Lets
// tests substitute a minimal fake without polyfilling the whole
// RTCPeerConnection API.
export interface PassthroughPeerSurface {
  addTrack(track: MediaStreamTrack, ...streams: MediaStream[]): RTCRtpSender;
  removeTrack(sender: RTCRtpSender): void;
}

/**
 * CameraPassthrough — controller for the camera/mic side of the
 * peer connection. One instance per Session.
 */
export class CameraPassthrough {
  private readonly opts: PassthroughOptions;
  private readonly pc: PassthroughPeerSurface;
  private state: PassthroughState = {
    enabled: false,
    videoTrackActive: false,
    audioTrackActive: false,
    stream: null,
  };
  // Track the senders so we can removeTrack on disable.
  private senders: RTCRtpSender[] = [];
  // Listeners on each track's "ended" event so a UA-driven stop
  // (e.g., user revoked permission via the URL bar) flows back to us.
  private trackEndDetachers: Array<() => void> = [];

  constructor(pc: PassthroughPeerSurface, opts: PassthroughOptions = {}) {
    this.pc = pc;
    this.opts = opts;
  }

  /** Snapshot of the current state. */
  getState(): PassthroughState {
    return { ...this.state };
  }

  /**
   * Acquire camera/mic and add the tracks to the peer connection.
   * Resolves to the live MediaStream, rejects with PassthroughError
   * on any failure. Idempotent — calling enable() while already
   * enabled returns the existing stream.
   *
   * MUST be called from a user-gesture handler (click) so the UA
   * permission prompt fires.
   */
  async enable(): Promise<MediaStream> {
    if (this.state.enabled && this.state.stream) {
      return this.state.stream;
    }

    const wantVideo = !this.opts.disableVideo;
    const wantAudio = !this.opts.disableAudio;
    if (!wantVideo && !wantAudio) {
      throw new PassthroughError(
        "no_track_requested",
        "both video and audio disabled — nothing to share",
      );
    }

    const constraints: MediaStreamConstraints = {};
    if (wantVideo) {
      constraints.video = this.opts.videoDeviceId
        ? { deviceId: { exact: this.opts.videoDeviceId } }
        : true;
    }
    if (wantAudio) {
      constraints.audio = this.opts.audioDeviceId
        ? { deviceId: { exact: this.opts.audioDeviceId } }
        : true;
    }

    const md = this.opts.mediaDevices ?? defaultMediaDevices();
    if (!md) {
      const err = new PassthroughError(
        "media_devices_unavailable",
        "navigator.mediaDevices is not available in this context",
      );
      this.opts.onError?.(err);
      throw err;
    }

    let stream: MediaStream;
    try {
      stream = await md.getUserMedia(constraints);
    } catch (e) {
      const err = mapGetUserMediaError(e);
      this.opts.onError?.(err);
      throw err;
    }

    // Add every track from the stream. addTrack() returns a sender
    // we'll need at disable time.
    for (const t of stream.getTracks()) {
      const sender = this.pc.addTrack(t, stream);
      this.senders.push(sender);

      // If a track ends out from under us (user revoked, device
      // unplugged, …) treat it as a partial disable.
      const onEnd = () => this.handleTrackEnd(t);
      t.addEventListener("ended", onEnd);
      this.trackEndDetachers.push(() => t.removeEventListener("ended", onEnd));
    }

    this.state = {
      enabled: true,
      videoTrackActive: wantVideo,
      audioTrackActive: wantAudio,
      stream,
    };
    this.opts.onStateChange?.(this.getState());
    this.opts.onNeedRenegotiate?.("track_added");
    return stream;
  }

  /**
   * Stop all tracks, remove them from the peer connection, and
   * trigger a renegotiation so the m= sections drop. Idempotent —
   * a no-op when not enabled.
   */
  disable(): void {
    if (!this.state.enabled) return;
    // Detach end listeners first so disable() doesn't recurse.
    for (const detach of this.trackEndDetachers) {
      try { detach(); } catch { /* ignore */ }
    }
    this.trackEndDetachers = [];

    // Stop tracks before removeTrack — this fires "ended" but the
    // listeners are already detached.
    if (this.state.stream) {
      for (const t of this.state.stream.getTracks()) {
        try { t.stop(); } catch { /* ignore */ }
      }
    }
    for (const s of this.senders) {
      try { this.pc.removeTrack(s); } catch { /* ignore */ }
    }
    this.senders = [];

    this.state = {
      enabled: false,
      videoTrackActive: false,
      audioTrackActive: false,
      stream: null,
    };
    this.opts.onStateChange?.(this.getState());
    this.opts.onNeedRenegotiate?.("track_removed");
  }

  /** Internal — called when a track fires "ended" on its own. */
  private handleTrackEnd(track: MediaStreamTrack): void {
    if (!this.state.enabled || !this.state.stream) return;
    // If only one track ended, update the partial state but leave
    // the rest. If everything is now ended, fully disable.
    const stillLive = this.state.stream.getTracks()
      .filter(t => t !== track && t.readyState === "live");
    if (stillLive.length === 0) {
      this.disable();
      return;
    }
    this.state = {
      ...this.state,
      videoTrackActive: stillLive.some(t => t.kind === "video"),
      audioTrackActive: stillLive.some(t => t.kind === "audio"),
    };
    this.opts.onStateChange?.(this.getState());
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function defaultMediaDevices(): MediaDevices | null {
  if (typeof navigator === "undefined") return null;
  if (!navigator.mediaDevices) return null;
  return navigator.mediaDevices;
}

/** Map a getUserMedia rejection to a PassthroughError with a code we
 *  expose to the UI. */
export function mapGetUserMediaError(e: unknown): PassthroughError {
  const name = (e as { name?: string } | null)?.name ?? "Unknown";
  const message = (e as { message?: string } | null)?.message ?? String(e);
  switch (name) {
    case "NotAllowedError":
    case "SecurityError":
      return new PassthroughError("permission_denied",
        "the user (or the UA's policy) denied access to camera/mic");
    case "NotFoundError":
    case "OverconstrainedError":
      return new PassthroughError("no_device",
        "no camera/mic available matching the requested constraints");
    case "NotReadableError":
    case "AbortError":
      return new PassthroughError("device_busy",
        "another application is already using the camera/mic");
    case "TypeError":
      return new PassthroughError("invalid_constraints",
        `getUserMedia constraints rejected: ${message}`);
    default:
      return new PassthroughError("internal_error", `${name}: ${message}`);
  }
}
