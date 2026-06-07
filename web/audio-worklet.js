// AudioWorklet processor for pokeemerald WASM audio output.
// The main thread pushes resampled Float32 L/R chunks via port.postMessage;
// this processor drains them into the browser's audio pipeline.

const RING_SIZE = 16384; // power-of-2, ~341 ms at 48 kHz
const RING_MASK = RING_SIZE - 1;

// Target latency: keep ~3 frames of audio buffered (avoid both underrun and
// growing latency). At 48 kHz / 60 fps each frame is ~800 output samples.
const TARGET_FILL = 2400;
const MAX_FILL = TARGET_FILL * 2;

class PokeemeraldAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this._L = new Float32Array(RING_SIZE);
    this._R = new Float32Array(RING_SIZE);
    this._write = 0;
    this._read = 0;

    this.port.onmessage = ({ data: { L, R } }) => {
      const avail = (this._write - this._read) & (RING_SIZE * 2 - 1);
      if (avail > MAX_FILL) return; // drop to prevent latency build-up
      for (let i = 0; i < L.length; i++) {
        this._L[this._write & RING_MASK] = L[i];
        this._R[this._write & RING_MASK] = R[i];
        this._write = (this._write + 1) & (RING_SIZE * 2 - 1);
      }
    };
  }

  process(_inputs, outputs) {
    const L = outputs[0][0];
    const R = outputs[0][1];
    const n = L.length;

    const avail = (this._write - this._read) & (RING_SIZE * 2 - 1);
    if (avail < n) {
      L.fill(0);
      R.fill(0);
    } else {
      for (let i = 0; i < n; i++) {
        L[i] = this._L[this._read & RING_MASK];
        R[i] = this._R[this._read & RING_MASK];
        this._read = (this._read + 1) & (RING_SIZE * 2 - 1);
      }
    }
    return true;
  }
}

registerProcessor('pokeemerald-audio', PokeemeraldAudioProcessor);
