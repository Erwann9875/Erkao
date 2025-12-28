const {
  createConnection,
  ProposedFeatures,
  TextDocuments,
  CompletionItemKind,
  SymbolKind,
  Location,
  Range,
  MarkupKind,
  DiagnosticSeverity,
  ResponseError,
  ErrorCodes,
  TextEdit
} = require("vscode-languageserver/node");
const { TextDocument } = require("vscode-languageserver-textdocument");
const path = require("path");
const fs = require("fs");
const cp = require("child_process");
const { fileURLToPath } = require("url");

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);
const typecheckTimers = new Map();
let workspaceRoots = [];
let clientSupportsConfig = false;
let settings = {
  diagnosticsMode: "change",
  debounceMs: 300,
  typecheckExecutable: ""
};

const KEYWORDS = [
  "let",
  "const",
  "fun",
  "class",
  "enum",
  "if",
  "else",
  "while",
  "for",
  "foreach",
  "switch",
  "match",
  "case",
  "default",
  "break",
  "continue",
  "import",
  "from",
  "as",
  "export",
  "return",
  "true",
  "false",
  "null",
  "this",
  "and",
  "or",
  "in"
];

const BUILTINS = [
  "print",
  "clock",
  "type",
  "len",
  "args",
  "push",
  "keys",
  "values"
];

const MODULES = [
  "fs",
  "path",
  "json",
  "math",
  "random",
  "str",
  "array",
  "os",
  "time",
  "http",
  "proc",
  "env",
  "plugin",
  "gfx"
];

const HOVER_DOCS = new Map([
  ["print", "`print(...)` built-in function."],
  ["clock", "`clock()` seconds since process start."],
  ["type", "`type(value)` returns the value type name."],
  ["len", "`len(value)` length of string/array/map."],
  ["args", "`args()` returns CLI args array."],
  ["push", "`push(array, value)` appends and returns new length."],
  ["keys", "`keys(map)` returns array of keys."],
  ["values", "`values(map)` returns array of values."],
  ["fs", "Stdlib module `fs` (filesystem helpers)."],
  ["path", "Stdlib module `path` (path helpers)."],
  ["json", "Stdlib module `json` (parse/stringify)."],
  ["math", "Stdlib module `math` (math helpers/constants)."],
  ["random", "Stdlib module `random` (PRNG helpers)."],
  ["str", "Stdlib module `str` (string helpers)."],
  ["array", "Stdlib module `array` (array helpers)."],
  ["os", "Stdlib module `os` (platform utilities)."],
  ["time", "Stdlib module `time` (time helpers)."],
  ["http", "Stdlib module `http` (HTTP client/server)."],
  ["proc", "Stdlib module `proc` (run commands)."],
  ["env", "Stdlib module `env` (environment helpers)."],
  ["plugin", "Stdlib module `plugin` (native plugins)."],
  ["gfx", "Stdlib module `gfx` (SDL2 graphics)."]
]);

const symbolCache = new Map();

function isIdentChar(ch) {
  return /[A-Za-z0-9_]/.test(ch);
}

function isIdentStart(ch) {
  return /[A-Za-z_]/.test(ch);
}

function isValidIdentifier(name) {
  return /^[A-Za-z_][A-Za-z0-9_]*$/.test(name);
}

function getWordAt(text, offset) {
  if (offset < 0 || offset >= text.length) return null;
  let start = offset;
  while (start > 0 && isIdentChar(text[start - 1])) {
    start--;
  }
  let end = offset;
  while (end < text.length && isIdentChar(text[end])) {
    end++;
  }
  if (start === end) return null;
  return { word: text.slice(start, end), start, end };
}

function getWordInfo(doc, position) {
  const text = doc.getText();
  let offset = doc.offsetAt(position);
  let info = getWordAt(text, offset);
  if (!info && offset > 0) {
    info = getWordAt(text, offset - 1);
  }
  return info;
}

function resolveErkaoExe() {
  const override = settings.typecheckExecutable || process.env.ERKAO_LSP_EXE;
  if (override) {
    if (fs.existsSync(override)) return override;
    return override;
  }
  for (const root of workspaceRoots) {
    const winCandidate = path.join(root, "build", "Debug", "erkao.exe");
    if (fs.existsSync(winCandidate)) return winCandidate;
    const winAlt = path.join(root, "build", "erkao.exe");
    if (fs.existsSync(winAlt)) return winAlt;
    const unixCandidate = path.join(root, "build", "erkao");
    if (fs.existsSync(unixCandidate)) return unixCandidate;
  }
  return "erkao";
}

async function loadSettings() {
  if (!clientSupportsConfig) return;
  try {
    const config = await connection.workspace.getConfiguration("erkao");
    if (config && typeof config === "object") {
      const lspConfig = config.lsp || {};
      if (typeof lspConfig.diagnosticsMode === "string") {
        settings.diagnosticsMode = lspConfig.diagnosticsMode;
      }
      if (typeof lspConfig.diagnosticsDebounceMs === "number") {
        settings.debounceMs = lspConfig.diagnosticsDebounceMs;
      }
      if (typeof lspConfig.typecheckExecutable === "string") {
        settings.typecheckExecutable = lspConfig.typecheckExecutable;
      }
    }
  } catch (err) {
  }
}

function shouldRunDiagnostics(trigger) {
  if (settings.diagnosticsMode === "off") return false;
  if (settings.diagnosticsMode === "save") {
    return trigger === "save";
  }
  return true;
}

function parseTypecheckDiagnostics(output, filePath) {
  const diagnostics = [];
  if (!output) return diagnostics;
  const normalizedPath = path.normalize(filePath);
  const lines = output.split(/\r?\n/);
  const regex = /^(.*?):(\d+):(\d+): Error.*?: (.*)$/;

  for (const line of lines) {
    const match = regex.exec(line);
    if (!match) continue;
    const entryPath = path.normalize(match[1]);
    if (entryPath !== normalizedPath) continue;
    const lineNum = Math.max(parseInt(match[2], 10) - 1, 0);
    const colNum = Math.max(parseInt(match[3], 10) - 1, 0);
    const message = match[4];
    const range = Range.create(lineNum, colNum, lineNum, colNum + 1);
    diagnostics.push({
      range,
      severity: DiagnosticSeverity.Error,
      source: "erkao typecheck",
      message
    });
  }

  return diagnostics;
}

function scheduleTypecheck(doc, trigger) {
  if (!doc || !doc.uri.startsWith("file://")) return;
  if (!doc.uri.endsWith(".ek")) return;
  if (!shouldRunDiagnostics(trigger)) return;
  const existing = typecheckTimers.get(doc.uri);
  if (existing) clearTimeout(existing);
  const delay = Math.max(0, settings.debounceMs || 0);
  const handle = setTimeout(() => runTypecheck(doc), delay);
  typecheckTimers.set(doc.uri, handle);
}

function runTypecheck(doc) {
  if (!doc || !doc.uri.startsWith("file://")) return;
  if (!doc.uri.endsWith(".ek")) return;
  const filePath = fileURLToPath(doc.uri);
  const exe = resolveErkaoExe();
  if (!exe) return;

  cp.execFile(
    exe,
    ["typecheck", filePath],
    { cwd: path.dirname(filePath) },
    (err, stdout, stderr) => {
      const output = `${stdout || ""}\n${stderr || ""}`;
      const diagnostics = parseTypecheckDiagnostics(output, filePath);
      connection.sendDiagnostics({ uri: doc.uri, diagnostics });
    }
  );
}

function skipWhitespace(text, index) {
  let i = index;
  while (i < text.length && /\s/.test(text[i])) {
    i++;
  }
  return i;
}

function extractTypeAnnotation(text, index) {
  let i = skipWhitespace(text, index);
  if (text[i] !== ":") return null;
  i++;
  i = skipWhitespace(text, i);
  const start = i;
  let depth = 0;
  while (i < text.length) {
    const ch = text[i];
    if (ch === "<") depth++;
    else if (ch === ">" && depth > 0) depth--;
    else if (depth === 0 &&
             (ch === "=" || ch === ";" || ch === "," || ch === ")" ||
              ch === "{" || ch === "\n" || ch === "\r")) {
      break;
    }
    i++;
  }
  const typeText = text.slice(start, i).trim();
  return typeText.length ? typeText : null;
}

function parseParamSegment(segment) {
  let part = segment.trim();
  if (!part) return null;
  const eqIndex = part.indexOf("=");
  if (eqIndex !== -1) {
    part = part.slice(0, eqIndex).trim();
  }
  if (!part) return null;
  const colonIndex = part.indexOf(":");
  if (colonIndex === -1) {
    return { name: part, type: null };
  }
  const name = part.slice(0, colonIndex).trim();
  const type = part.slice(colonIndex + 1).trim();
  if (!name) return null;
  return { name, type: type || null };
}

function parseParamList(text, startIndex) {
  let i = startIndex;
  let depthParen = 0;
  let depthBracket = 0;
  let depthBrace = 0;
  let depthAngle = 0;
  let inString = false;
  let stringTriple = false;
  let segmentStart = i;
  const segments = [];

  while (i < text.length) {
    const ch = text[i];
    const next = text[i + 1];
    const next2 = text[i + 2];

    if (inString) {
      if (stringTriple) {
        if (ch === "\"" && next === "\"" && next2 === "\"") {
          inString = false;
          stringTriple = false;
          i += 3;
          continue;
        }
        i++;
        continue;
      }
      if (ch === "\\") {
        i += 2;
        continue;
      }
      if (ch === "\"") {
        inString = false;
        i++;
        continue;
      }
      i++;
      continue;
    }

    if (ch === "\"") {
      if (next === "\"" && next2 === "\"") {
        inString = true;
        stringTriple = true;
        i += 3;
        continue;
      }
      inString = true;
      stringTriple = false;
      i++;
      continue;
    }

    if (ch === "(") {
      depthParen++;
      i++;
      continue;
    }
    if (ch === ")") {
      if (depthParen === 0 && depthBracket === 0 && depthBrace === 0) {
        segments.push(text.slice(segmentStart, i));
        return { segments, endIndex: i };
      }
      if (depthParen > 0) depthParen--;
      i++;
      continue;
    }
    if (ch === "[") { depthBracket++; i++; continue; }
    if (ch === "]") { if (depthBracket > 0) depthBracket--; i++; continue; }
    if (ch === "{") { depthBrace++; i++; continue; }
    if (ch === "}") { if (depthBrace > 0) depthBrace--; i++; continue; }
    if (ch === "<") { depthAngle++; i++; continue; }
    if (ch === ">") { if (depthAngle > 0) depthAngle--; i++; continue; }

    if (ch === "," && depthParen === 0 && depthBracket === 0 &&
        depthBrace === 0 && depthAngle === 0) {
      segments.push(text.slice(segmentStart, i));
      i++;
      segmentStart = i;
      continue;
    }

    i++;
  }
  return null;
}

function buildFunctionDetail(text, name, nameEnd) {
  let i = skipWhitespace(text, nameEnd);
  if (text[i] !== "(") return null;
  const parsed = parseParamList(text, i + 1);
  if (!parsed) return null;

  const params = parsed.segments
    .map(parseParamSegment)
    .filter(Boolean);
  const returnType = extractTypeAnnotation(text, parsed.endIndex + 1);
  const hasTypes = params.some((param) => param.type) || returnType;
  if (!hasTypes) return null;

  const signatureParams = params.map((param) => {
    if (param.type) return `${param.name}: ${param.type}`;
    return param.name;
  }).join(", ");
  const returnSuffix = returnType ? `: ${returnType}` : "";
  return `fun ${name}(${signatureParams})${returnSuffix}`;
}

function findIdentifierOccurrences(text, target) {
  const ranges = [];
  if (!target) return ranges;

  let i = 0;
  let inLineComment = false;
  let inBlockComment = false;
  let inString = false;
  let stringTriple = false;
  let inInterpolation = false;
  let interpDepth = 0;
  let resumeString = false;
  let resumeStringTriple = false;

  while (i < text.length) {
    const ch = text[i];
    const next = text[i + 1];
    const next2 = text[i + 2];

    if (inLineComment) {
      if (ch === "\n") inLineComment = false;
      i++;
      continue;
    }
    if (inBlockComment) {
      if (ch === "*" && next === "/") {
        inBlockComment = false;
        i += 2;
        continue;
      }
      i++;
      continue;
    }

    if (inString) {
      if (!inInterpolation && ch === "$" && next === "{") {
        inInterpolation = true;
        interpDepth = 0;
        resumeString = true;
        resumeStringTriple = stringTriple;
        inString = false;
        i += 2;
        continue;
      }
      if (stringTriple) {
        if (ch === "\"" && next === "\"" && next2 === "\"") {
          inString = false;
          stringTriple = false;
          i += 3;
          continue;
        }
        i++;
        continue;
      }
      if (ch === "\\") {
        i += 2;
        continue;
      }
      if (ch === "\"") {
        inString = false;
        i++;
        continue;
      }
      i++;
      continue;
    }

    if (ch === "/" && next === "/") {
      inLineComment = true;
      i += 2;
      continue;
    }
    if (ch === "/" && next === "*") {
      inBlockComment = true;
      i += 2;
      continue;
    }
    if (ch === "\"") {
      if (next === "\"" && next2 === "\"") {
        inString = true;
        stringTriple = true;
        i += 3;
        continue;
      }
      inString = true;
      stringTriple = false;
      i++;
      continue;
    }

    if (inInterpolation) {
      if (ch === "{") {
        interpDepth++;
        i++;
        continue;
      }
      if (ch === "}") {
        if (interpDepth === 0) {
          inInterpolation = false;
          if (resumeString) {
            inString = true;
            stringTriple = resumeStringTriple;
            resumeString = false;
          }
          i++;
          continue;
        }
        interpDepth--;
        i++;
        continue;
      }
    }

    if (isIdentStart(ch)) {
      const start = i;
      i++;
      while (i < text.length && isIdentChar(text[i])) {
        i++;
      }
      if (text.slice(start, i) === target) {
        ranges.push({ start, end: i });
      }
      continue;
    }

    i++;
  }

  return ranges;
}

function rangesToLocations(doc, ranges) {
  return ranges.map((range) => {
    const start = doc.positionAt(range.start);
    const end = doc.positionAt(range.end);
    return Location.create(doc.uri, Range.create(start, end));
  });
}

function rangesToEdits(doc, ranges, newText) {
  return ranges.map((range) => {
    const start = doc.positionAt(range.start);
    const end = doc.positionAt(range.end);
    return TextEdit.replace(Range.create(start, end), newText);
  });
}

function addSymbol(doc, symbols, byName, name, kind, nameIndex, detail) {
  const start = doc.positionAt(nameIndex);
  const end = doc.positionAt(nameIndex + name.length);
  const range = Range.create(start, end);
  const symbol = { name, kind, range, selectionRange: range };
  if (detail) symbol.detail = detail;
  symbols.push(symbol);
  if (!byName.has(name)) {
    byName.set(name, { range, kind, detail });
  }
}

function parseSymbols(doc) {
  const text = doc.getText();
  const symbols = [];
  const byName = new Map();

  const classRegex = /\bclass\s+([A-Za-z_][A-Za-z0-9_]*)/g;
  let match = null;
  while ((match = classRegex.exec(text)) !== null) {
    const name = match[1];
    const nameIndex = match.index + match[0].indexOf(name);
    addSymbol(doc, symbols, byName, name, SymbolKind.Class, nameIndex, null);
  }

  const funRegex = /\bfun\s+([A-Za-z_][A-Za-z0-9_]*)/g;
  while ((match = funRegex.exec(text)) !== null) {
    const name = match[1];
    const nameIndex = match.index + match[0].indexOf(name);
    const detail = buildFunctionDetail(text, name, nameIndex + name.length);
    addSymbol(doc, symbols, byName, name, SymbolKind.Function, nameIndex, detail);
  }

  const letRegex = /\blet\s+([A-Za-z_][A-Za-z0-9_]*)/g;
  while ((match = letRegex.exec(text)) !== null) {
    const name = match[1];
    const nameIndex = match.index + match[0].indexOf(name);
    const type = extractTypeAnnotation(text, nameIndex + name.length);
    const detail = type ? `${name}: ${type}` : null;
    addSymbol(doc, symbols, byName, name, SymbolKind.Variable, nameIndex, detail);
  }

  const constRegex = /\bconst\s+([A-Za-z_][A-Za-z0-9_]*)/g;
  while ((match = constRegex.exec(text)) !== null) {
    const name = match[1];
    const nameIndex = match.index + match[0].indexOf(name);
    const type = extractTypeAnnotation(text, nameIndex + name.length);
    const detail = type ? `${name}: ${type}` : null;
    addSymbol(doc, symbols, byName, name, SymbolKind.Variable, nameIndex, detail);
  }

  const enumRegex = /\benum\s+([A-Za-z_][A-Za-z0-9_]*)/g;
  while ((match = enumRegex.exec(text)) !== null) {
    const name = match[1];
    const nameIndex = match.index + match[0].indexOf(name);
    addSymbol(doc, symbols, byName, name, SymbolKind.Enum, nameIndex, null);
  }

  return { symbols, byName };
}

function getSymbols(doc) {
  const cached = symbolCache.get(doc.uri);
  if (cached && cached.version === doc.version) return cached;
  const parsed = parseSymbols(doc);
  const entry = { version: doc.version, symbols: parsed.symbols, byName: parsed.byName };
  symbolCache.set(doc.uri, entry);
  return entry;
}

documents.onDidClose((event) => {
  symbolCache.delete(event.document.uri);
  const handle = typecheckTimers.get(event.document.uri);
  if (handle) clearTimeout(handle);
  typecheckTimers.delete(event.document.uri);
  connection.sendDiagnostics({ uri: event.document.uri, diagnostics: [] });
});

documents.onDidChangeContent((event) => {
  scheduleTypecheck(event.document, "change");
});

documents.onDidSave((event) => {
  scheduleTypecheck(event.document, "save");
});

connection.onInitialize((params) => {
  workspaceRoots = [];
  if (params && Array.isArray(params.workspaceFolders)) {
    for (const folder of params.workspaceFolders) {
      if (folder && folder.uri && folder.uri.startsWith("file://")) {
        workspaceRoots.push(fileURLToPath(folder.uri));
      }
    }
  } else if (params && params.rootUri && params.rootUri.startsWith("file://")) {
    workspaceRoots.push(fileURLToPath(params.rootUri));
  }
  clientSupportsConfig = !!(params &&
    params.capabilities &&
    params.capabilities.workspace &&
    params.capabilities.workspace.configuration);
  return {
    capabilities: {
      textDocumentSync: documents.syncKind,
      completionProvider: { resolveProvider: false, triggerCharacters: ["."] },
      hoverProvider: true,
      definitionProvider: true,
      documentSymbolProvider: true,
      referencesProvider: true,
      renameProvider: { prepareProvider: true }
    }
  };
});

connection.onInitialized(() => {
  if (clientSupportsConfig) {
    loadSettings().then(() => {
      if (settings.diagnosticsMode === "off") return;
      for (const doc of documents.all()) {
        scheduleTypecheck(doc, "save");
      }
    });
  }
});

connection.onDidChangeConfiguration(() => {
  if (!clientSupportsConfig) return;
  loadSettings().then(() => {
    if (settings.diagnosticsMode === "off") {
      for (const doc of documents.all()) {
        connection.sendDiagnostics({ uri: doc.uri, diagnostics: [] });
      }
      return;
    }
    for (const doc of documents.all()) {
      scheduleTypecheck(doc, "save");
    }
  });
});

connection.onCompletion((params) => {
  const doc = documents.get(params.textDocument.uri);
  const items = [];

  KEYWORDS.forEach((kw) => {
    items.push({ label: kw, kind: CompletionItemKind.Keyword });
  });

  BUILTINS.forEach((fn) => {
    items.push({ label: fn, kind: CompletionItemKind.Function });
  });

  MODULES.forEach((mod) => {
    items.push({ label: mod, kind: CompletionItemKind.Module });
  });

  if (doc) {
    const { byName } = getSymbols(doc);
    for (const [name, info] of byName.entries()) {
      let kind = CompletionItemKind.Variable;
      if (info.kind === SymbolKind.Class) kind = CompletionItemKind.Class;
      if (info.kind === SymbolKind.Function) kind = CompletionItemKind.Function;
      items.push({ label: name, kind });
    }
  }

  return items;
});

connection.onHover((params) => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const wordInfo = getWordInfo(doc, params.position);
  if (!wordInfo) return null;

  const docText = HOVER_DOCS.get(wordInfo.word);
  if (docText) {
    return {
      contents: {
        kind: MarkupKind.Markdown,
        value: docText
      }
    };
  }

  if (KEYWORDS.includes(wordInfo.word)) {
    return { contents: { kind: MarkupKind.Markdown, value: `Keyword \`${wordInfo.word}\`.` } };
  }

  const { byName } = getSymbols(doc);
  const info = byName.get(wordInfo.word);
  if (info && info.detail) {
    return {
      contents: {
        kind: MarkupKind.Markdown,
        value: `\`${info.detail}\``
      }
    };
  }

  return null;
});

connection.onDefinition((params) => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const wordInfo = getWordInfo(doc, params.position);
  if (!wordInfo) return null;

  const { byName } = getSymbols(doc);
  const info = byName.get(wordInfo.word);
  if (!info) return null;
  return Location.create(doc.uri, info.range);
});

connection.onReferences((params) => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const wordInfo = getWordInfo(doc, params.position);
  if (!wordInfo || KEYWORDS.includes(wordInfo.word)) return [];

  const results = [];
  for (const openDoc of documents.all()) {
    const ranges = findIdentifierOccurrences(openDoc.getText(), wordInfo.word);
    const { byName } = getSymbols(openDoc);
    const def = byName.get(wordInfo.word);
    const includeDecl = params.context ? params.context.includeDeclaration : true;
    if (!includeDecl && def) {
      for (let i = ranges.length - 1; i >= 0; i--) {
        const start = openDoc.positionAt(ranges[i].start);
        const end = openDoc.positionAt(ranges[i].end);
        if (Range.create(start, end).start.line === def.range.start.line &&
            Range.create(start, end).start.character === def.range.start.character &&
            Range.create(start, end).end.line === def.range.end.line &&
            Range.create(start, end).end.character === def.range.end.character) {
          ranges.splice(i, 1);
        }
      }
    }
    results.push(...rangesToLocations(openDoc, ranges));
  }
  return results;
});

connection.onPrepareRename((params) => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const wordInfo = getWordInfo(doc, params.position);
  if (!wordInfo) return null;
  if (KEYWORDS.includes(wordInfo.word)) return null;
  const start = doc.positionAt(wordInfo.start);
  const end = doc.positionAt(wordInfo.end);
  return Range.create(start, end);
});

connection.onRenameRequest((params) => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const wordInfo = getWordInfo(doc, params.position);
  if (!wordInfo || KEYWORDS.includes(wordInfo.word)) return null;
  if (!isValidIdentifier(params.newName) || KEYWORDS.includes(params.newName)) {
    throw new ResponseError(
      ErrorCodes.InvalidParams,
      "Rename target must be a valid identifier."
    );
  }

  const changes = {};
  for (const openDoc of documents.all()) {
    const ranges = findIdentifierOccurrences(openDoc.getText(), wordInfo.word);
    if (ranges.length === 0) continue;
    changes[openDoc.uri] = rangesToEdits(openDoc, ranges, params.newName);
  }

  return { changes };
});

connection.onDocumentSymbol((params) => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  return getSymbols(doc).symbols;
});

documents.listen(connection);
connection.listen();
