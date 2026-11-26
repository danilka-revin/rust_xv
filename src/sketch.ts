/**
 * Arduino-style sketch preparation.
 *
 * A `.ino` file is not valid C++ on its own: the Arduino IDE wraps it into a
 * translation unit, adds `#include <Arduino.h>` and generates prototypes for
 * the functions the author wrote (so `loop()` may call a helper defined below).
 * The browser toolchain compiles one file, so we do exactly that here and keep
 * enough bookkeeping to map compiler line numbers back onto the user's file.
 */

export interface PreparedSketch {
  /** Source handed to the compiler. */
  code: string;
  /** How many lines were prepended (the implicit Arduino.h include). */
  headerLines: number;
  /** How many generated prototype lines were inserted. */
  protoCount: number;
  /** All lines added before the user's own code (include, spacer, prototypes). */
  generatedLines: number;
}

const NOT_A_FUNCTION = new Set([
  'if', 'for', 'while', 'switch', 'return', 'else', 'do', 'case', 'new', 'delete',
  'sizeof', 'catch', 'try', 'throw', 'class', 'struct', 'union', 'enum', 'namespace',
  'template', 'extern', 'typedef', 'using', 'static_assert', 'default', 'goto',
]);

/** `ret name(args)` where ret is a plausible return type and the line opens a definition. */
const FUNCTION_HEAD = /^(?<ret>(?:(?:static|inline|constexpr|extern)\s+)*[A-Za-z_][\w:<>,\s\*&\[\]]*?)\s+(?<name>[A-Za-z_]\w*(?:::\w+)?)\s*\((?<args>[^;{}]*)\)\s*(?:const\s*)?(?:noexcept\s*)?\{\s*\}?\s*$/;

interface LineInfo {
  text: string;
  depth: number;
}

/** Split source into lines and record the brace depth at the start of each line. */
function scanLines(source: string): LineInfo[] {
  const lines: LineInfo[] = [];
  let depth = 0;
  // Brace depth at the moment the current line started: that is what decides
  // whether a definition sits at file scope.
  let lineDepth = 0;
  let current = '';
  let state: 'code' | 'line' | 'block' | 'string' | 'char' = 'code';

  const push = (text: string) => {
    lines.push({ text, depth: lineDepth });
    lineDepth = depth;
  };

  for (let i = 0; i < source.length; i++) {
    const ch = source[i];
    const next = source[i + 1];

    if (state === 'line') {
      if (ch === '\n') {
        push(current);
        current = '';
        state = 'code';
      } else {
        current += ch;
      }
      continue;
    }

    if (state === 'block') {
      current += ch;
      if (ch === '*' && next === '/') {
        current += next;
        i++;
        state = 'code';
      } else if (ch === '\n') {
        push(current);
        current = '';
      }
      continue;
    }

    if (state === 'string' || state === 'char') {
      const quote = state === 'string' ? '"' : "'";
      current += ch;
      if (ch === '\\') {
        current += source[i + 1] ?? '';
        i++;
      } else if (ch === quote) {
        state = 'code';
      } else if (ch === '\n') {
        state = 'code';
        push(current);
        current = '';
      }
      continue;
    }

    // Plain code.
    if (ch === '/' && next === '/') {
      state = 'line';
      current += ch;
      continue;
    }
    if (ch === '/' && next === '*') {
      state = 'block';
      current += ch;
      continue;
    }
    if (ch === '"') {
      state = 'string';
      current += ch;
      continue;
    }
    if (ch === "'") {
      state = 'char';
      current += ch;
      continue;
    }
    if (ch === '\n') {
      push(current);
      current = '';
      continue;
    }
    if (ch === '{') depth++;
    if (ch === '}') depth = Math.max(0, depth - 1);
    current += ch;
  }
  if (current) push(current);
  return lines;
}

/** Collect prototypes for top-level functions defined with a body. */
export function collectPrototypes(source: string): string[] {
  const prototypes: string[] = [];
  const seen = new Set<string>();

  for (const { text, depth } of scanLines(source)) {
    if (depth !== 0) continue;
    const line = text.trim();
    if (!line || line.startsWith('#') || line.startsWith('//') || line.startsWith('*')) continue;
    if (line.includes('=') && line.indexOf('=') < line.indexOf('(')) continue;

    const match = FUNCTION_HEAD.exec(line);
    if (!match?.groups) continue;
    const ret = match.groups.ret.replace(/\s+/g, ' ').trim();
    const name = match.groups.name;
    const args = match.groups.args.trim();
    if (NOT_A_FUNCTION.has(ret.split(' ').pop() ?? '')) continue;
    if (NOT_A_FUNCTION.has(name)) continue;
    if (/^(operator|template)\b/.test(name)) continue;

    const prototype = `${ret} ${name}(${args});`;
    if (seen.has(prototype)) continue;
    seen.add(prototype);
    prototypes.push(prototype);
  }
  return prototypes;
}

export function prepareSketch(source: string): PreparedSketch {
  const body = source.replace(/^\uFEFF/, '').replace(/\r\n?/g, '\n');
  const headerLines = 1;
  const prototypes = collectPrototypes(body);

  let insertAfter = 0;
  const lines = body.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const trimmed = lines[i].trim();
    const isPreprocessor = trimmed.startsWith('#');
    const isComment = trimmed.startsWith('//') || trimmed.startsWith('/*') || trimmed.startsWith('*') || trimmed.endsWith('*/');
    if (trimmed === '' || isPreprocessor || isComment) {
      insertAfter = i + 1;
      continue;
    }
    break;
  }

  const head = ['#include <Arduino.h>'];
  const rest = [...lines];
  if (prototypes.length) rest.splice(insertAfter, 0, '', ...prototypes);

  return {
    code: [...head, ...rest].join('\n') + (body.endsWith('\n') || body === '' ? '' : '\n'),
    headerLines,
    protoCount: prototypes.length,
    generatedLines: headerLines + (prototypes.length ? prototypes.length + 1 : 0),
  };
}

/** Map a compiler line number back to the line in the user's file (0 when generated). */
export function mapLine(line: number, prepared: PreparedSketch): number {
  if (line <= prepared.generatedLines) return 0;
  return line - prepared.generatedLines;
}

/**
 * Merge several `.ino` files the way the Arduino IDE does: the file named after
 * the sketch first, the rest alphabetically, in one translation unit.
 */
export function mergeInoTabs(files: { name: string; content: string }[], mainName: string): string {
  const ordered = [
    ...files.filter((f) => f.name === mainName),
    ...files.filter((f) => f.name !== mainName).sort((a, b) => a.name.localeCompare(b.name)),
  ];
  return ordered.map((f) => `// ${f.name}\n${f.content.replace(/\s*$/, '\n')}`).join('\n');
}
