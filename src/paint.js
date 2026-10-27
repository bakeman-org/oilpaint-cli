/**
 * Oil-paint style transfer.
 *
 * The painterly base comes from a Kuwahara filter: every pixel picks the
 * mean color of the surrounding region with the lowest variance, which
 * flattens skin/foliage into brush-like patches while keeping edges sharp.
 * A canvas weave and grain are layered on top. Both the brush and the
 * texture scale with image size so the style reads the same at any resolution.
 */

// Brush radius is expressed at a 400px reference edge; it scales with the
// image so the effect stays visible on large photos instead of shrinking
// into a few pixels.
const REFERENCE_EDGE = 400;

export function oilPaintPixels(rgb, width, height, options = {}) {
  const radius = options.radius ?? 4;
  const texture = options.texture ?? 0.35;
  const saturation = options.saturation ?? 1;

  const brush = Math.max(1, Math.round((radius * Math.max(width, height)) / REFERENCE_EDGE));
  const painted = kuwahara(rgb, width, height, brush);
  applySaturation(painted, saturation);
  if (texture > 0) applyCanvasTexture(painted, width, height, texture);
  return painted;
}

function kuwahara(src, width, height, radius) {
  const stride = width + 1;
  const plane = stride * (height + 1);

  // Summed-area tables let each quadrant be measured in constant time.
  const sumR = new Float64Array(plane);
  const sumG = new Float64Array(plane);
  const sumB = new Float64Array(plane);
  const sqR = new Float64Array(plane);
  const sqG = new Float64Array(plane);
  const sqB = new Float64Array(plane);

  for (let y = 0; y < height; y++) {
    let rowR = 0;
    let rowG = 0;
    let rowB = 0;
    let rowSqR = 0;
    let rowSqG = 0;
    let rowSqB = 0;
    const srcRow = y * width * 3;
    const current = (y + 1) * stride;
    const previous = y * stride;

    for (let x = 0; x < width; x++) {
      const p = srcRow + x * 3;
      const r = src[p];
      const g = src[p + 1];
      const b = src[p + 2];

      rowR += r;
      rowG += g;
      rowB += b;
      rowSqR += r * r;
      rowSqG += g * g;
      rowSqB += b * b;

      const i = current + x + 1;
      const up = previous + x + 1;
      sumR[i] = sumR[up] + rowR;
      sumG[i] = sumG[up] + rowG;
      sumB[i] = sumB[up] + rowB;
      sqR[i] = sqR[up] + rowSqR;
      sqG[i] = sqG[up] + rowSqG;
      sqB[i] = sqB[up] + rowSqB;
    }
  }

  const out = new Uint8ClampedArray(width * height * 3);

  // Reused coordinate buffers to avoid per-pixel allocations.
  const qx0 = new Int32Array(4);
  const qx1 = new Int32Array(4);
  const qy0 = new Int32Array(4);
  const qy1 = new Int32Array(4);

  for (let y = 0; y < height; y++) {
    const top0 = Math.max(0, y - radius);
    const top1 = y + 1;
    const bottom0 = y;
    const bottom1 = Math.min(height, y + radius + 1);

    for (let x = 0; x < width; x++) {
      const left0 = Math.max(0, x - radius);
      const left1 = x + 1;
      const right0 = x;
      const right1 = Math.min(width, x + radius + 1);

      qx0[0] = left0;
      qx1[0] = left1;
      qy0[0] = top0;
      qy1[0] = top1;

      qx0[1] = right0;
      qx1[1] = right1;
      qy0[1] = top0;
      qy1[1] = top1;

      qx0[2] = left0;
      qx1[2] = left1;
      qy0[2] = bottom0;
      qy1[2] = bottom1;

      qx0[3] = right0;
      qx1[3] = right1;
      qy0[3] = bottom0;
      qy1[3] = bottom1;

      let bestVariance = Infinity;
      let bestR = 0;
      let bestG = 0;
      let bestB = 0;

      for (let q = 0; q < 4; q++) {
        const x0 = qx0[q];
        const x1 = qx1[q];
        const y0 = qy0[q];
        const y1 = qy1[q];
        const count = (x1 - x0) * (y1 - y0);
        if (count === 0) continue;

        const a = y0 * stride + x0;
        const b = y0 * stride + x1;
        const c = y1 * stride + x0;
        const d = y1 * stride + x1;

        const meanR = (sumR[d] - sumR[b] - sumR[c] + sumR[a]) / count;
        const meanG = (sumG[d] - sumG[b] - sumG[c] + sumG[a]) / count;
        const meanB = (sumB[d] - sumB[b] - sumB[c] + sumB[a]) / count;

        const varR = Math.max(0, (sqR[d] - sqR[b] - sqR[c] + sqR[a]) / count - meanR * meanR);
        const varG = Math.max(0, (sqG[d] - sqG[b] - sqG[c] + sqG[a]) / count - meanG * meanG);
        const varB = Math.max(0, (sqB[d] - sqB[b] - sqB[c] + sqB[a]) / count - meanB * meanB);

        const variance = varR + varG + varB;
        if (variance < bestVariance) {
          bestVariance = variance;
          bestR = meanR;
          bestG = meanG;
          bestB = meanB;
        }
      }

      const o = (y * width + x) * 3;
      out[o] = bestR + 0.5;
      out[o + 1] = bestG + 0.5;
      out[o + 2] = bestB + 0.5;
    }
  }

  return out;
}

function applySaturation(pixels, saturation) {
  if (!Number.isFinite(saturation) || saturation === 1) return;

  for (let i = 0; i < pixels.length; i += 3) {
    const r = pixels[i];
    const g = pixels[i + 1];
    const b = pixels[i + 2];
    const luma = 0.299 * r + 0.587 * g + 0.114 * b;

    pixels[i] = luma + (r - luma) * saturation;
    pixels[i + 1] = luma + (g - luma) * saturation;
    pixels[i + 2] = luma + (b - luma) * saturation;
  }
}

function applyCanvasTexture(pixels, width, height, strength) {
  const longEdge = Math.max(width, height);
  const amplitude = strength * 30;
  // Weave and blotch sizes scale with the image; fixed frequencies would
  // average out to nothing once the photo is viewed at screen size.
  const weavePeriod = Math.max(6, longEdge / 90);
  const weaveFreq = (Math.PI * 2) / weavePeriod;
  const broadScale = Math.max(8, longEdge / 45);

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      // Diagonal linen weave.
      const weave =
        Math.sin(x * weaveFreq + y * weaveFreq * 0.15) *
        Math.sin(y * weaveFreq - x * weaveFreq * 0.15);
      // Fine bristle grain plus a broader paint unevenness.
      const grain = hash2(x, y) - 0.5;
      const broad = smoothNoise(x, y, broadScale) - 0.5;
      const delta = amplitude * (weave * 0.3 + grain * 0.35 + broad * 0.5);

      const i = (y * width + x) * 3;
      pixels[i] += delta;
      pixels[i + 1] += delta;
      pixels[i + 2] += delta;
    }
  }
}

function hash2(x, y) {
  let h = Math.imul(x, 374761393) + Math.imul(y, 668265263);
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}

function smoothNoise(x, y, scale) {
  const fx = x / scale;
  const fy = y / scale;
  const ix = Math.floor(fx);
  const iy = Math.floor(fy);
  const tx = fx - ix;
  const ty = fy - iy;
  const sx = tx * tx * (3 - 2 * tx);
  const sy = ty * ty * (3 - 2 * ty);

  const n00 = hash2(ix, iy);
  const n10 = hash2(ix + 1, iy);
  const n01 = hash2(ix, iy + 1);
  const n11 = hash2(ix + 1, iy + 1);

  const top = n00 + (n10 - n00) * sx;
  const bottom = n01 + (n11 - n01) * sx;
  return top + (bottom - top) * sy;
}
