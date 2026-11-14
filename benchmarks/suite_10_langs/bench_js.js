'use strict';

const os = require('os');
const { Worker, isMainThread, workerData } = require('worker_threads');

class Node {
  constructor(x, y, z) {
    this.x = x;
    this.y = y;
    this.z = z;
  }
}

function fib(n) {
  if (n <= 1) return n;
  return fib(n - 1) + fib(n - 2);
}

function stepFn(a, b) {
  let next = a + b;
  if (next >= 1000003) next -= 1000003;
  return next;
}

function nowUs() {
  return Number(process.hrtime.bigint() / 1000n);
}

if (!isMainThread) {
  const { start, end, cols, sab } = workerData;
  const rowSums = new Float64Array(sab);
  for (let r = start; r < end; r++) {
    let acc = r;
    for (let c = 0; c < cols; c++) {
      acc += 7919;
      if (acc >= 1000003) acc -= 1000003;
    }
    rowSums[r] = acc;
  }
  process.exit(0);
}

async function run() {
  const tAll = nowUs();

  // Task 1 (strict f32 ops via Math.fround)
  const n1 = 100000000;
  const step1 = Math.fround(0.00000001);
  let sum1 = Math.fround(0.0);
  let x1 = Math.fround(0.000000005);
  const four = Math.fround(4.0);
  const one = Math.fround(1.0);
  const t1_0 = nowUs();
  for (let i = 0; i < n1; i++) {
    sum1 = Math.fround(sum1 + Math.fround(four / Math.fround(one + Math.fround(x1 * x1))));
    x1 = Math.fround(x1 + step1);
  }
  const us1 = nowUs() - t1_0;
  const pi1 = Math.fround(sum1 * step1);

  // Task 2 (8-byte elements using Float64Array, exact <= 2^53 ints)
  const n2 = 50000000;
  const t2_0 = nowUs();
  let arr2 = new Float64Array(n2);
  let cur2 = 1;
  for (let i = 0; i < n2; i++) {
    cur2 += 7;
    if (cur2 >= 1024) cur2 -= 1024;
    arr2[i] = cur2;
  }
  let sum2 = 0;
  for (let i = 0; i < n2; i++) {
    const v = arr2[i] * 3 + 1;
    arr2[i] = v;
    sum2 += v;
  }
  arr2 = null;
  const us2 = nowUs() - t2_0;

  // Task 3
  const n3 = 1000000;
  const t3_0 = nowUs();
  let nodes3 = new Array(n3);
  for (let i = 0; i < n3; i++) {
    nodes3[i] = new Node(i, i * 2, i + 5);
  }
  let sum3 = 0;
  for (let i = 0; i < n3; i++) {
    const p = nodes3[i];
    sum3 += p.x + p.y + p.z;
  }
  nodes3 = null;
  const us3 = nowUs() - t3_0;

  // Task 4
  const t4_0 = nowUs();
  const f40 = fib(40);
  let acc4 = 0;
  for (let i = 0; i < 100000000; i++) {
    acc4 = stepFn(acc4, 7919);
  }
  const us4 = nowUs() - t4_0;
  const chk4 = f40 + acc4;

  // Task 5
  const rows5 = 2000;
  const cols5 = 50000;
  const t5_0 = nowUs();
  const sab = new SharedArrayBuffer(rows5 * 8);
  const rowSums = new Float64Array(sab);
  const workers = Math.max(1, os.cpus().length);
  const chunk = Math.floor(rows5 / workers);
  const rem = rows5 % workers;
  let cur = 0;
  const promises = [];
  for (let w = 0; w < workers; w++) {
    const sz = chunk + (w < rem ? 1 : 0);
    const start = cur;
    const end = cur + sz;
    cur = end;
    promises.push(new Promise((resolve, reject) => {
      const worker = new Worker(__filename, {
        workerData: { start, end, cols: cols5, sab }
      });
      worker.on('error', reject);
      worker.on('exit', code => code === 0 ? resolve() : reject(new Error('Worker exit ' + code)));
    }));
  }
  await Promise.all(promises);
  let sum5 = 0;
  for (let r = 0; r < rows5; r++) sum5 += rowSums[r];
  const us5 = nowUs() - t5_0;

  const usTotal = nowUs() - tAll;

  console.log(`TASK1_US ${us1}`);
  console.log(`TASK1_PI ${pi1.toFixed(6)}`);
  console.log(`TASK2_US ${us2}`);
  console.log(`TASK2_CHK ${sum2}`);
  console.log(`TASK3_US ${us3}`);
  console.log(`TASK3_CHK ${sum3}`);
  console.log(`TASK4_US ${us4}`);
  console.log(`TASK4_CHK ${chk4}`);
  console.log(`TASK5_US ${us5}`);
  console.log(`TASK5_CHK ${sum5}`);
  console.log(`TOTAL_US ${usTotal}`);
}

run();
