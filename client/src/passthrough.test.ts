// Unit tests for client/src/passthrough.ts.
//
// We substitute fake MediaDevices, MediaStream, MediaStreamTrack, and
// RTCPeerConnection surfaces so the tests run without a real browser.

import { describe, it, expect, vi } from "vitest";
import {
  CameraPassthrough,
  PassthroughError,
  mapGetUserMediaError,
  PassthroughPeerSurface,
} from "./passthrough.js";

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

class FakeTrack {
  readonly kind: "video" | "audio";
  readyState: "live" | "ended" = "live";
  private listeners: Array<() => void> = [];

  constructor(kind: "video" | "audio") {
    this.kind = kind;
  }
  addEventListener(type: string, cb: () => void): void {
    if (type === "ended") this.listeners.push(cb);
  }
  removeEventListener(type: string, cb: () => void): void {
    if (type !== "ended") return;
    this.listeners = this.listeners.filter(l => l !== cb);
  }
  stop(): void {
    if (this.readyState === "ended") return;
    this.readyState = "ended";
    this.fireEnded();
  }
  /** Test-helper — simulate a UA-driven end (revoked permission). */
  simulateEnded(): void {
    this.readyState = "ended";
    this.fireEnded();
  }
  private fireEnded(): void {
    for (const cb of [...this.listeners]) cb();
  }
}

class FakeStream {
  private readonly tracks: FakeTrack[];
  constructor(tracks: FakeTrack[]) { this.tracks = tracks; }
  getTracks(): FakeTrack[] { return [...this.tracks]; }
}

class FakeMediaDevices {
  constructor(
    public readonly getUserMedia: (c: MediaStreamConstraints) => Promise<MediaStream>,
  ) {}
}

class FakeSender { constructor(public readonly track: FakeTrack) {} }

class FakePC implements PassthroughPeerSurface {
  added: Array<{ track: FakeTrack; streams: FakeStream[] }> = [];
  removed: FakeSender[] = [];
  // RTCRtpSender's full type is huge; we cast to satisfy the interface.
  addTrack(track: MediaStreamTrack, ...streams: MediaStream[]): RTCRtpSender {
    const ft = track as unknown as FakeTrack;
    const fs = streams as unknown as FakeStream[];
    this.added.push({ track: ft, streams: fs });
    return new FakeSender(ft) as unknown as RTCRtpSender;
  }
  removeTrack(s: RTCRtpSender): void {
    this.removed.push(s as unknown as FakeSender);
  }
}

function buildPassthrough(opts: {
  tracks?: FakeTrack[];
  rejectWith?: unknown;
} = {}) {
  const tracks = opts.tracks ?? [new FakeTrack("video"), new FakeTrack("audio")];
  const stream = new FakeStream(tracks);
  const md = new FakeMediaDevices(async () => {
    if (opts.rejectWith !== undefined) throw opts.rejectWith;
    return stream as unknown as MediaStream;
  });
  const pc = new FakePC();
  const onState = vi.fn();
  const onNeed = vi.fn();
  const onErr = vi.fn();
  const cp = new CameraPassthrough(pc, {
    mediaDevices: md as unknown as MediaDevices,
    onStateChange: onState,
    onNeedRenegotiate: onNeed,
    onError: onErr,
  });
  return { cp, pc, stream, tracks, onState, onNeed, onErr };
}

// ---------------------------------------------------------------------------
// happy path
// ---------------------------------------------------------------------------

describe("CameraPassthrough.enable / disable", () => {
  it("enable adds tracks, fires onStateChange + onNeedRenegotiate(track_added)", async () => {
    const { cp, pc, onState, onNeed } = buildPassthrough();
    const stream = await cp.enable();
    expect(stream).toBeDefined();
    expect(pc.added).toHaveLength(2);
    expect(pc.added.map(a => a.track.kind).sort()).toEqual(["audio", "video"]);

    expect(onState).toHaveBeenCalled();
    const last = onState.mock.calls.at(-1)![0];
    expect(last.enabled).toBe(true);
    expect(last.videoTrackActive).toBe(true);
    expect(last.audioTrackActive).toBe(true);

    expect(onNeed).toHaveBeenCalledWith("track_added");
  });

  it("enable is idempotent", async () => {
    const { cp, pc } = buildPassthrough();
    await cp.enable();
    await cp.enable();
    // Second call should NOT add tracks again.
    expect(pc.added).toHaveLength(2);
  });

  it("disable stops tracks, removes senders, fires onNeedRenegotiate(track_removed)", async () => {
    const { cp, pc, tracks, onNeed } = buildPassthrough();
    await cp.enable();
    onNeed.mockClear();

    cp.disable();

    expect(pc.removed).toHaveLength(2);
    for (const t of tracks) {
      expect(t.readyState).toBe("ended");
    }
    expect(onNeed).toHaveBeenCalledWith("track_removed");
    expect(cp.getState().enabled).toBe(false);
  });

  it("disable is a no-op when not enabled", () => {
    const { cp, pc, onNeed } = buildPassthrough();
    cp.disable();
    expect(pc.removed).toHaveLength(0);
    expect(onNeed).not.toHaveBeenCalled();
  });
});

// ---------------------------------------------------------------------------
// constraints + partial requests
// ---------------------------------------------------------------------------

describe("CameraPassthrough constraints", () => {
  it("disableVideo:true requests audio only", async () => {
    let seenConstraints: MediaStreamConstraints | null = null;
    const md = {
      getUserMedia: async (c: MediaStreamConstraints) => {
        seenConstraints = c;
        return new FakeStream([new FakeTrack("audio")]) as unknown as MediaStream;
      },
    };
    const cp = new CameraPassthrough(new FakePC(), {
      mediaDevices: md as unknown as MediaDevices,
      disableVideo: true,
    });
    await cp.enable();
    expect(seenConstraints).not.toBeNull();
    expect(seenConstraints!.video).toBeUndefined();
    expect(seenConstraints!.audio).toBe(true);
  });

  it("rejects when both video and audio are disabled", async () => {
    const cp = new CameraPassthrough(new FakePC(), {
      mediaDevices: { getUserMedia: async () => new FakeStream([]) as unknown as MediaStream } as unknown as MediaDevices,
      disableVideo: true, disableAudio: true,
    });
    await expect(cp.enable()).rejects.toMatchObject({ code: "no_track_requested" });
  });

  it("uses exact-deviceId constraint when supplied", async () => {
    let seen: MediaStreamConstraints | null = null;
    const md = {
      getUserMedia: async (c: MediaStreamConstraints) => {
        seen = c;
        return new FakeStream([new FakeTrack("video")]) as unknown as MediaStream;
      },
    };
    const cp = new CameraPassthrough(new FakePC(), {
      mediaDevices: md as unknown as MediaDevices,
      videoDeviceId: "cam-1",
      disableAudio: true,
    });
    await cp.enable();
    expect(seen!.video).toEqual({ deviceId: { exact: "cam-1" } });
  });
});

// ---------------------------------------------------------------------------
// error mapping
// ---------------------------------------------------------------------------

describe("mapGetUserMediaError", () => {
  it("NotAllowedError → permission_denied", () => {
    const e = mapGetUserMediaError({ name: "NotAllowedError", message: "user denied" });
    expect(e.code).toBe("permission_denied");
  });
  it("NotFoundError → no_device", () => {
    const e = mapGetUserMediaError({ name: "NotFoundError", message: "no cam" });
    expect(e.code).toBe("no_device");
  });
  it("NotReadableError → device_busy", () => {
    const e = mapGetUserMediaError({ name: "NotReadableError", message: "in use" });
    expect(e.code).toBe("device_busy");
  });
  it("anything else → internal_error", () => {
    const e = mapGetUserMediaError({ name: "Unicorn", message: "?!" });
    expect(e.code).toBe("internal_error");
  });
});

describe("CameraPassthrough error path", () => {
  it("calls onError and rejects when getUserMedia rejects", async () => {
    const { cp, onErr, onNeed } = buildPassthrough({
      rejectWith: { name: "NotAllowedError", message: "no" },
    });
    await expect(cp.enable()).rejects.toBeInstanceOf(PassthroughError);
    expect(onErr).toHaveBeenCalled();
    expect(onErr.mock.calls[0]![0].code).toBe("permission_denied");
    // No renegotiation should have been requested because no tracks were added.
    expect(onNeed).not.toHaveBeenCalled();
  });
});

// ---------------------------------------------------------------------------
// UA-driven track end
// ---------------------------------------------------------------------------

describe("CameraPassthrough UA-driven track end", () => {
  it("partial-end (one of two tracks) updates state but stays enabled", async () => {
    const v = new FakeTrack("video");
    const a = new FakeTrack("audio");
    const { cp, onState } = buildPassthrough({ tracks: [v, a] });
    await cp.enable();
    onState.mockClear();

    v.simulateEnded();

    expect(cp.getState().enabled).toBe(true);
    expect(cp.getState().videoTrackActive).toBe(false);
    expect(cp.getState().audioTrackActive).toBe(true);
    expect(onState).toHaveBeenCalled();
  });

  it("full end (all tracks) auto-disables", async () => {
    const v = new FakeTrack("video");
    const a = new FakeTrack("audio");
    const { cp, pc, onNeed } = buildPassthrough({ tracks: [v, a] });
    await cp.enable();
    onNeed.mockClear();

    // End them in sequence — the first leaves us partially-enabled,
    // the second triggers the disable() path.
    v.simulateEnded();
    a.simulateEnded();

    expect(cp.getState().enabled).toBe(false);
    expect(pc.removed.length).toBeGreaterThan(0);
    expect(onNeed).toHaveBeenCalledWith("track_removed");
  });
});
