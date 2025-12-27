const {
  createConnection,
  ProposedFeatures,
  TextDocuments,
  CompletionItemKind,
  SymbolKind,
  Location,
  Range,
  MarkupKind
} = require("vscode-languageserver/node");
const { TextDocument } = require("vscode-languageserver-textdocument");

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);

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

function addSymbol(doc, symbols, byName, name, kind, nameIndex) {
  const start = doc.positionAt(nameIndex);
  const end = doc.positionAt(nameIndex + name.length);
  const range = Range.create(start, end);
  symbols.push({ name, kind, range, selectionRange: range });
  if (!byName.has(name)) {
    byName.set(name, { range, kind });
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
    addSymbol(doc, symbols, byName, name, SymbolKind.Class, nameIndex);
  }

  const funRegex = /\bfun\s+([A-Za-z_][A-Za-z0-9_]*)/g;
  while ((match = funRegex.exec(text)) !== null) {
    const name = match[1];
    const nameIndex = match.index + match[0].indexOf(name);
    addSymbol(doc, symbols, byName, name, SymbolKind.Function, nameIndex);
  }

  const letRegex = /\blet\s+([A-Za-z_][A-Za-z0-9_]*)/g;
  while ((match = letRegex.exec(text)) !== null) {
    const name = match[1];
    const nameIndex = match.index + match[0].indexOf(name);
    addSymbol(doc, symbols, byName, name, SymbolKind.Variable, nameIndex);
  }

  const constRegex = /\bconst\s+([A-Za-z_][A-Za-z0-9_]*)/g;
  while ((match = constRegex.exec(text)) !== null) {
    const name = match[1];
    const nameIndex = match.index + match[0].indexOf(name);
    addSymbol(doc, symbols, byName, name, SymbolKind.Variable, nameIndex);
  }

  const enumRegex = /\benum\s+([A-Za-z_][A-Za-z0-9_]*)/g;
  while ((match = enumRegex.exec(text)) !== null) {
    const name = match[1];
    const nameIndex = match.index + match[0].indexOf(name);
    addSymbol(doc, symbols, byName, name, SymbolKind.Enum, nameIndex);
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
});

connection.onInitialize(() => {
  return {
    capabilities: {
      textDocumentSync: documents.syncKind,
      completionProvider: { resolveProvider: false, triggerCharacters: ["."] },
      hoverProvider: true,
      definitionProvider: true,
      documentSymbolProvider: true
    }
  };
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

connection.onDocumentSymbol((params) => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  return getSymbols(doc).symbols;
});

documents.listen(connection);
connection.listen();
