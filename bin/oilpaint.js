#!/usr/bin/env node
import { parseArgs } from 'node:util';
import { readFile, mkdir, stat } from 'node:fs/promises';
import path from 'node:path';
import sharp from 'sharp';
import { oilPaintPixels } from '../src/paint.js';

const FORMATS = {
  png: 'png',
  jpg: 'jpeg',
  jpeg: 'jpeg',
  webp: 'webp',
};

const EXTENSIONS = {
  png: 'png',
  jpeg: 'jpg',
  webp: 'webp',
};

const HELP = `oilpaint - turn photos into oil paintings

Usage:
  oilpaint [options] <image...>

Options:
  -o, --out <path>        Output file or directory (default: ./oilpaint-output)
  -r, --radius <n>        Brush size, 1-24 (default: 4, scales with image size)
  -m, --max <px>          Max long edge before painting, 64+ (default: 1600)
      --texture <0-1>     Canvas and bristle texture strength (default: 0.35)
  -s, --saturation <n>    Saturation multiplier, 0-3 (default: 1)
  -f, --format <fmt>      Output format: png, jpeg or webp (default: input format)
  -q, --quality <1-100>   JPEG/WebP quality (default: 92)
  -c, --concurrency <n>   Images processed in parallel, 1-8 (default: 2)
  -h, --help              Show this help
  -v, --version           Show version

Examples:
  oilpaint photo.jpg
  oilpaint -r 6 --texture 0.5 -o paintings/ *.jpg
  oilpaint -f webp -s 1.2 -o out.webp portrait.png
`;

function fail(message) {
  console.error(`oilpaint: ${message}`);
  console.error('Run "oilpaint --help" for usage.');
  process.exit(2);
}

function parseNumber(value, name, fallback, constraints = {}) {
  if (value === undefined) return fallback;
  const { min = -Infinity, max = Infinity, integer = false } = constraints;
  const number = Number(value);
  if (
    value === '' ||
    !Number.isFinite(number) ||
    (integer && !Number.isInteger(number)) ||
    number < min ||
    number > max
  ) {
    fail(`invalid value for --${name}: "${value}"`);
  }
  return number;
}

function formatFromPath(filePath) {
  const ext = path.extname(filePath).slice(1).toLowerCase();
  return FORMATS[ext] ?? null;
}

function stripExtension(filePath) {
  return path.basename(filePath, path.extname(filePath));
}

async function isDirectory(target) {
  try {
    return (await stat(target)).isDirectory();
  } catch {
    return false;
  }
}

async function planJobs(inputs, outOption, explicitFormat) {
  const defaultDir = path.resolve('oilpaint-output');
  const outIsDir =
    outOption === undefined ||
    inputs.length > 1 ||
    outOption.endsWith(path.sep) ||
    (await isDirectory(outOption));

  return inputs.map((input) => {
    const resolvedInput = path.resolve(input);
    const inputFormat = formatFromPath(input) ?? 'png';

    if (outOption === undefined) {
      const format = explicitFormat ?? inputFormat;
      return {
        input: resolvedInput,
        format,
        output: path.join(defaultDir, `${stripExtension(input)}-oilpaint.${EXTENSIONS[format]}`),
      };
    }

    if (outIsDir) {
      const format = explicitFormat ?? inputFormat;
      return {
        input: resolvedInput,
        format,
        output: path.join(
          path.resolve(outOption),
          `${stripExtension(input)}-oilpaint.${EXTENSIONS[format]}`,
        ),
      };
    }

    let output = path.resolve(outOption);
    const outputExt = path.extname(output);
    const outputFormat = outputExt === '' ? null : FORMATS[outputExt.slice(1).toLowerCase()] ?? null;
    const format = explicitFormat ?? outputFormat ?? inputFormat;

    if (outputFormat === null) {
      output += `.${EXTENSIONS[format]}`;
    } else if (explicitFormat && outputFormat !== explicitFormat) {
      output = output.slice(0, -outputExt.length) + `.${EXTENSIONS[format]}`;
    }

    return { input: resolvedInput, format, output };
  });
}

function toRgb(data, channels, width, height) {
  if (channels === 3) return data;

  const rgb = new Uint8Array(width * height * 3);

  if (channels === 1) {
    for (let i = 0, j = 0; i < data.length; i++, j += 3) {
      rgb[j] = data[i];
      rgb[j + 1] = data[i];
      rgb[j + 2] = data[i];
    }
    return rgb;
  }

  for (let i = 0, j = 0; i < data.length; i += channels, j += 3) {
    rgb[j] = data[i];
    rgb[j + 1] = data[i + 1];
    rgb[j + 2] = data[i + 2];
  }
  return rgb;
}

async function processJob(job, options) {
  const startedAt = Date.now();

  const { data, info } = await sharp(job.input, { failOn: 'none' })
    .rotate()
    .flatten({ background: '#ffffff' })
    .toColourspace('srgb')
    .resize({
      width: options.max,
      height: options.max,
      fit: 'inside',
      withoutEnlargement: true,
      kernel: 'lanczos3',
    })
    .raw()
    .toBuffer({ resolveWithObject: true });

  const rgb = toRgb(data, info.channels, info.width, info.height);
  const painted = oilPaintPixels(rgb, info.width, info.height, {
    radius: options.radius,
    texture: options.texture,
    saturation: options.saturation,
  });

  const encoder = sharp(painted, {
    raw: { width: info.width, height: info.height, channels: 3 },
  });

  if (job.format === 'jpeg') {
    encoder.jpeg({ quality: options.quality, mozjpeg: true });
  } else if (job.format === 'webp') {
    encoder.webp({ quality: options.quality });
  } else {
    encoder.png({ compressionLevel: 9, adaptiveFiltering: true });
  }

  await mkdir(path.dirname(job.output), { recursive: true });
  await encoder.toFile(job.output);

  const elapsed = ((Date.now() - startedAt) / 1000).toFixed(1);
  console.log(`${job.input} -> ${job.output} (${info.width}x${info.height}, ${elapsed}s)`);
}

async function runJobs(jobs, options) {
  let cursor = 0;
  const failures = [];

  const workers = Array.from(
    { length: Math.min(options.concurrency, jobs.length) },
    async () => {
      while (cursor < jobs.length) {
        const job = jobs[cursor++];
        try {
          await processJob(job, options);
        } catch (error) {
          failures.push(`${job.input}: ${error.message}`);
        }
      }
    },
  );

  await Promise.all(workers);
  return failures;
}

async function main() {
  let values;
  let positionals;

  try {
    ({ values, positionals } = parseArgs({
      allowPositionals: true,
      options: {
        out: { type: 'string', short: 'o' },
        radius: { type: 'string', short: 'r' },
        max: { type: 'string', short: 'm' },
        texture: { type: 'string' },
        saturation: { type: 'string', short: 's' },
        format: { type: 'string', short: 'f' },
        quality: { type: 'string', short: 'q' },
        concurrency: { type: 'string', short: 'c' },
        help: { type: 'boolean', short: 'h' },
        version: { type: 'boolean', short: 'v' },
      },
    }));
  } catch (error) {
    fail(error.message);
  }

  if (values.version) {
    const pkg = JSON.parse(await readFile(new URL('../package.json', import.meta.url), 'utf8'));
    console.log(pkg.version);
    return;
  }

  if (values.help || positionals.length === 0) {
    console.log(HELP);
    process.exit(values.help ? 0 : 2);
  }

  const options = {
    radius: parseNumber(values.radius, 'radius', 4, { min: 1, max: 24, integer: true }),
    max: parseNumber(values.max, 'max', 1600, { min: 64, integer: true }),
    texture: parseNumber(values.texture, 'texture', 0.35, { min: 0, max: 1 }),
    saturation: parseNumber(values.saturation, 'saturation', 1, { min: 0, max: 3 }),
    quality: parseNumber(values.quality, 'quality', 92, { min: 1, max: 100, integer: true }),
    concurrency: parseNumber(values.concurrency, 'concurrency', 2, { min: 1, max: 8, integer: true }),
  };

  let explicitFormat;
  if (values.format !== undefined) {
    explicitFormat = FORMATS[values.format.toLowerCase()];
    if (!explicitFormat) fail(`unsupported format "${values.format}" (use png, jpeg or webp)`);
  }

  const existing = [];
  for (const input of positionals) {
    const info = await stat(input).catch(() => null);
    if (!info || !info.isFile()) {
      console.error(`oilpaint: input not found: ${input}`);
      process.exitCode = 1;
      continue;
    }
    existing.push(input);
  }

  if (existing.length === 0) return;

  const jobs = await planJobs(existing, values.out, explicitFormat);
  const failures = await runJobs(jobs, options);

  for (const failure of failures) console.error(`oilpaint: failed: ${failure}`);
  if (failures.length > 0) process.exitCode = 1;
}

main().catch((error) => {
  console.error(`oilpaint: ${error.message}`);
  process.exitCode = 1;
});
