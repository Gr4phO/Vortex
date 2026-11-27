const os = require("os");
const { Worker, isMainThread, parentPort, workerData } = require("worker_threads");

if (!isMainThread) {
    const { y0, y1, w5, iter5, sab } = workerData;
    const out = new BigInt64Array(sab);
    for (let py = y0; py < y1; py++) {
        const cy = -1.25 + (py * 2.5) / w5;
        let rsum = 0;
        for (let px = 0; px < w5; px++) {
            const cx = -2.0 + (px * 3.0) / w5;
            let zr = 0.0, zi = 0.0, it = 0;
            while (it < iter5 && (zr * zr + zi * zi <= 4.0)) {
                const tmp = zr * zr - zi * zi + cx;
                zi = 2.0 * zr * zi + cy;
                zr = tmp;
                it++;
            }
            rsum += it;
        }
        out[py] = BigInt(rsum);
    }
    parentPort.postMessage(1);
    process.exit(0);
}

function qsortI64(arr, lo, hi) {
    let i = lo, j = hi;
    const pivot = arr[(lo + hi) >> 1];
    while (i <= j) {
        while (arr[i] < pivot) i++;
        while (arr[j] > pivot) j--;
        if (i <= j) {
            const tmp = arr[i];
            arr[i] = arr[j];
            arr[j] = tmp;
            i++;
            j--;
        }
    }
    if (lo < j) qsortI64(arr, lo, j);
    if (i < hi) qsortI64(arr, i, hi);
}

function nowUs() {
    return Number(process.hrtime.bigint() / 1000n);
}

async function main() {
    const n1 = 600, steps1 = 25;
    const n2 = 500000;
    const n3 = 320;
    const n4 = 10000000;
    const w5 = 1024, iter5 = 256;

    const tAll0 = nowUs();

    // T1
    const x = new Float64Array(n1), y = new Float64Array(n1), z = new Float64Array(n1);
    const vx = new Float64Array(n1), vy = new Float64Array(n1), vz = new Float64Array(n1), m = new Float64Array(n1);
    for (let i = 0; i < n1; i++) {
        x[i] = ((i * 17) % 100) * 0.1 - 5.0;
        y[i] = ((i * 31) % 100) * 0.1 - 5.0;
        z[i] = ((i * 47) % 100) * 0.1 - 5.0;
        vx[i] = ((i % 7) - 3) * 0.01;
        vy[i] = (((i * 3) % 7) - 3) * 0.01;
        vz[i] = (((i * 5) % 7) - 3) * 0.01;
        m[i] = 0.5 + (i % 5) * 0.25;
    }
    const dt = 0.01, soft = 0.01;
    const t1_0 = nowUs();
    for (let s = 0; s < steps1; s++) {
        for (let i = 0; i < n1; i++) {
            let fx = 0.0, fy = 0.0, fz = 0.0;
            const xi = x[i], yi = y[i], ziv = z[i];
            for (let j = 0; j < n1; j++) {
                if (j !== i) {
                    const dx = x[j] - xi;
                    const dy = y[j] - yi;
                    const dz = z[j] - ziv;
                    const d2 = dx * dx + dy * dy + dz * dz + soft;
                    const inv = 1.0 / Math.sqrt(d2);
                    const f = m[j] * inv * inv * inv;
                    fx += dx * f;
                    fy += dy * f;
                    fz += dz * f;
                }
            }
            vx[i] += fx * dt;
            vy[i] += fy * dt;
            vz[i] += fz * dt;
        }
        for (let i = 0; i < n1; i++) {
            x[i] += vx[i] * dt;
            y[i] += vy[i] * dt;
            z[i] += vz[i] * dt;
        }
    }
    let chk1 = 0.0;
    for (let i = 0; i < n1; i++) {
        chk1 += 0.5 * m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
    }
    const us1 = nowUs() - t1_0;

    // T2
    const arr2 = new Float64Array(n2);
    let lcg = 12345;
    for (let i = 0; i < n2; i++) {
        lcg = (lcg * 48271) % 2147483647;
        arr2[i] = lcg;
    }
    const t2_0 = nowUs();
    qsortI64(arr2, 0, n2 - 1);
    const chk2 = arr2[0] + arr2[250000] + arr2[n2 - 1];
    const us2 = nowUs() - t2_0;

    // T3
    const a3 = new Float64Array(n3 * n3), b3 = new Float64Array(n3 * n3), c3 = new Float64Array(n3 * n3);
    for (let i = 0; i < n3; i++) {
        for (let k = 0; k < n3; k++) {
            a3[i * n3 + k] = ((i * 7 + k * 13) % 97) * 0.01;
            b3[i * n3 + k] = ((i * 11 + k * 5) % 89) * 0.01;
        }
    }
    const t3_0 = nowUs();
    for (let i = 0; i < n3; i++) {
        const coff = i * n3;
        for (let k = 0; k < n3; k++) {
            const aik = a3[coff + k];
            const boff = k * n3;
            for (let j = 0; j < n3; j++) {
                c3[coff + j] += aik * b3[boff + j];
            }
        }
    }
    let chk3 = 0.0;
    for (let i = 0; i < n3 * n3; i++) chk3 += c3[i];
    const us3 = nowUs() - t3_0;

    // T4
    const isComp = new Int32Array(n4);
    const t4_0 = nowUs();
    for (let p = 2; p * p < n4; p++) {
        if (isComp[p] === 0) {
            for (let m4 = p * p; m4 < n4; m4 += p) isComp[m4] = 1;
        }
    }
    let chk4 = 0n;
    for (let i = 2; i < n4; i++) {
        if (isComp[i] === 0) chk4 += BigInt(i);
    }
    const us4 = nowUs() - t4_0;

    // T5
    const sab = new SharedArrayBuffer(w5 * 8);
    const rowSums = new BigInt64Array(sab);
    const workers = Math.max(1, os.cpus().length);
    const t5_0 = nowUs();
    const chunk = Math.floor(w5 / workers), rem = w5 % workers;
    let cur = 0;
    const promises = [];
    for (let w = 0; w < workers; w++) {
        const sz = chunk + (w < rem ? 1 : 0);
        const y0 = cur, y1 = cur + sz;
        cur += sz;
        promises.push(new Promise((resolve, reject) => {
            const wk = new Worker(__filename, { workerData: { y0, y1, w5, iter5, sab } });
            wk.on("message", () => resolve());
            wk.on("error", reject);
        }));
    }
    await Promise.all(promises);
    let chk5 = 0n;
    for (let i = 0; i < w5; i++) chk5 += rowSums[i];
    const us5 = nowUs() - t5_0;

    const totalUs = nowUs() - tAll0;
    console.log(`T1_US ${us1}\nT1_CHK ${chk1.toFixed(6)}`);
    console.log(`T2_US ${us2}\nT2_CHK ${chk2}`);
    console.log(`T3_US ${us3}\nT3_CHK ${chk3.toFixed(6)}`);
    console.log(`T4_US ${us4}\nT4_CHK ${chk4.toString()}`);
    console.log(`T5_US ${us5}\nT5_CHK ${chk5.toString()}`);
    console.log(`TOTAL_US ${totalUs}`);
}

main();
