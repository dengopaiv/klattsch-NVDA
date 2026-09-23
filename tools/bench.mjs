// Baseline measurement of the JavaScript engine: compile time and render
// speed as a multiple of real time. This is the number the C port must beat,
// and the one that decides whether speed is a reason for the port at all.
import { compileString, FormantSynth } from '../src/engine/index.js';

const SR = 48000;

// A screen reader speaks a line at a time. This is a typical one, in ARPABET.
const LINE = "DH AH K W IH K B R AW N F AA K S JH AH M P S OW V ER DH AH L EY Z IY D AO G .";
// A paragraph: what say-all hands over in bulk.
const PARA = Array(12).fill(LINE).join(' ');

function bench(label, text, iters) {
  // Compile
  let t0 = process.hrtime.bigint();
  let compiled;
  for (let i = 0; i < iters; i++) compiled = compileString(text);
  let t1 = process.hrtime.bigint();
  const compileMs = Number(t1 - t0) / 1e6 / iters;

  const { schedule, totalMs } = compiled;
  const samples = Math.ceil(totalMs * SR / 1000);

  // Render
  t0 = process.hrtime.bigint();
  for (let i = 0; i < iters; i++) {
    const synth = new FormantSynth({ sampleRate: SR, schedule });
    const buf = new Float32Array(samples);
    synth.process(buf);
  }
  t1 = process.hrtime.bigint();
  const renderMs = Number(t1 - t0) / 1e6 / iters;

  const audioMs = totalMs;
  const xRT = audioMs / renderMs;

  console.log(`${label}`);
  console.log(`  audio length   ${(audioMs / 1000).toFixed(2)} s  (${samples} samples @ ${SR} Hz)`);
  console.log(`  schedule       ${schedule.length} events`);
  console.log(`  compile        ${compileMs.toFixed(3)} ms`);
  console.log(`  render         ${renderMs.toFixed(2)} ms`);
  console.log(`  => ${xRT.toFixed(1)}x real time`);
  console.log(`  => time to first audio if rendered whole: ${(compileMs + renderMs).toFixed(1)} ms`);
  console.log();
  return { xRT, compileMs, renderMs, audioMs };
}

console.log(`node ${process.version}\n`);

// warm up the JIT
for (let i = 0; i < 20; i++) {
  const { schedule, totalMs } = compileString(LINE);
  const s = new FormantSynth({ sampleRate: SR, schedule });
  s.process(new Float32Array(Math.ceil(totalMs * SR / 1000)));
}

const line = bench('one line', LINE, 50);
const para = bench('one paragraph (12 lines)', PARA, 10);

// What a 20 ms chunk costs, which is the chunked-rendering case.
const { schedule } = compileString(PARA);
const synth = new FormantSynth({ sampleRate: SR, schedule });
const chunk = new Float32Array(Math.round(SR * 0.02));
let t0 = process.hrtime.bigint();
for (let i = 0; i < 500; i++) synth.process(chunk);
let t1 = process.hrtime.bigint();
const chunkMs = Number(t1 - t0) / 1e6 / 500;
console.log(`20 ms chunk render: ${chunkMs.toFixed(3)} ms  => ${(20 / chunkMs).toFixed(0)}x real time`);
console.log(`time to first audio, chunked: ${(para.compileMs + chunkMs).toFixed(1)} ms`);
