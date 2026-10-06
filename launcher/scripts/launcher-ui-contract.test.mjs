import assert from "node:assert/strict";
import test from "node:test";
import { readFile } from "node:fs/promises";
import { parse } from "svelte/compiler";

const appSource = await readFile(new URL("../src/App.svelte", import.meta.url), "utf8");
const rustSource = await readFile(new URL("../src-tauri/src/main.rs", import.meta.url), "utf8");
const ast = parse(appSource);

function walk(node, visit) {
  if (!node || typeof node !== "object") return;
  visit(node);
  for (const value of Object.values(node)) {
    if (Array.isArray(value)) {
      for (const item of value) walk(item, visit);
    } else {
      walk(value, visit);
    }
  }
}

function attribute(node, name) {
  return node.attributes?.find((entry) => entry.type === "Attribute" && entry.name === name) ?? null;
}

function staticAttributeText(node, name) {
  const value = attribute(node, name)?.value;
  if (!Array.isArray(value) || value.length !== 1 || value[0]?.type !== "Text") return null;
  return value[0].data;
}

function functionDeclarations() {
  const functions = new Map();
  for (const statement of ast.instance?.content?.body ?? []) {
    if (statement.type === "FunctionDeclaration" && statement.id?.name) {
      functions.set(statement.id.name, statement);
    }
  }
  return functions;
}

function parameterName(parameter) {
  if (!parameter) return null;
  if (parameter.type === "Identifier") return parameter.name;
  if (parameter.type === "AssignmentPattern" && parameter.left?.type === "Identifier") {
    return parameter.left.name;
  }
  return null;
}

function splitRustArguments(text) {
  const parts = [];
  let start = 0;
  let angle = 0;
  let paren = 0;
  let bracket = 0;
  let brace = 0;
  for (let index = 0; index < text.length; index += 1) {
    const char = text[index];
    if (char === "<") angle += 1;
    else if (char === ">") angle = Math.max(0, angle - 1);
    else if (char === "(") paren += 1;
    else if (char === ")") paren = Math.max(0, paren - 1);
    else if (char === "[") bracket += 1;
    else if (char === "]") bracket = Math.max(0, bracket - 1);
    else if (char === "{") brace += 1;
    else if (char === "}") brace = Math.max(0, brace - 1);
    else if (char === "," && angle === 0 && paren === 0 && bracket === 0 && brace === 0) {
      parts.push(text.slice(start, index).trim());
      start = index + 1;
    }
  }
  const tail = text.slice(start).trim();
  if (tail) parts.push(tail);
  return parts.filter(Boolean);
}

function snakeToCamel(value) {
  return value.replace(/_([a-z])/g, (_, letter) => letter.toUpperCase());
}

function rustCommandContracts() {
  const contracts = new Map();
  const pattern = /#\[tauri::command\]\s*(?:async\s+)?fn\s+([A-Za-z0-9_]+)\s*\(([\s\S]*?)\)\s*(?:->|\{)/g;
  for (const match of rustSource.matchAll(pattern)) {
    const [, name, argumentsText] = match;
    const publicArguments = [];
    for (const argument of splitRustArguments(argumentsText)) {
      const separator = argument.indexOf(":");
      if (separator < 0) continue;
      const argumentName = argument.slice(0, separator).trim();
      const argumentType = argument.slice(separator + 1).trim();
      if (argumentType.includes("tauri::State") || argumentType.includes("tauri::AppHandle")) continue;
      publicArguments.push(snakeToCamel(argumentName.replace(/^_+/, "")));
    }
    contracts.set(name, publicArguments.sort());
  }
  return contracts;
}

function registeredCommands() {
  const match = rustSource.match(/\.invoke_handler\(tauri::generate_handler!\[([\s\S]*?)\]\)/);
  assert.ok(match, "Rust invoke_handler registration was not found");
  return new Set(
    match[1]
      .split(",")
      .map((value) => value.trim())
      .filter(Boolean),
  );
}

function frontendInvocations() {
  const invocations = [];
  walk(ast.instance?.content, (node) => {
    if (node.type !== "CallExpression" || node.callee?.type !== "Identifier" || node.callee.name !== "invoke") return;
    const command = node.arguments?.[0];
    assert.equal(command?.type, "Literal", "invoke() command names must be string literals");
    assert.equal(typeof command.value, "string", "invoke() command names must be strings");

    const argumentObject = node.arguments?.[1];
    const keys = [];
    if (argumentObject) {
      assert.equal(argumentObject.type, "ObjectExpression", `invoke(${command.value}) arguments must be an object literal`);
      for (const property of argumentObject.properties) {
        assert.equal(property.type, "Property", `invoke(${command.value}) must not use spread arguments`);
        assert.equal(property.computed, false, `invoke(${command.value}) must not use computed argument names`);
        const key = property.key?.name ?? property.key?.value;
        assert.equal(typeof key, "string", `invoke(${command.value}) has an unsupported argument name`);
        keys.push(key);
      }
    }
    invocations.push({ command: command.value, keys: keys.sort(), line: node.loc?.start?.line ?? 0 });
  });
  return invocations;
}

test("direct UI event bindings cannot accidentally receive non-event arguments", () => {
  const functions = functionDeclarations();
  const eventAttributes = new Set(["onclick", "onchange", "oninput"]);
  walk(ast.html, (node) => {
    if (node.type !== "Element") return;
    for (const entry of node.attributes ?? []) {
      if (entry.type !== "Attribute" || !eventAttributes.has(entry.name)) continue;
      const expression = entry.value?.[0]?.expression;
      if (expression?.type !== "Identifier") continue;
      const declaration = functions.get(expression.name);
      assert.ok(declaration, `${entry.name} references unknown handler ${expression.name}`);
      if (declaration.params.length === 0) continue;
      const first = parameterName(declaration.params[0]);
      assert.equal(
        first,
        "event",
        `${entry.name}={${expression.name}} passes the DOM event as argument 1, but ${expression.name} expects ${first ?? "a non-event parameter"}`,
      );
    }
  });
});

test("all launcher buttons are explicit button controls with a real action", () => {
  let buttons = 0;
  walk(ast.html, (node) => {
    if (node.type !== "Element" || node.name !== "button") return;
    buttons += 1;
    assert.equal(staticAttributeText(node, "type"), "button", `button at offset ${node.start} must declare type=\"button\"`);
    const clickable = Boolean(attribute(node, "onclick"));
    assert.ok(clickable, `button at offset ${node.start} has no onclick action`);
  });
  assert.ok(buttons >= 30, `expected the launcher button inventory, found only ${buttons}`);
});

test("all editable launcher controls expose an explicit disabled state", () => {
  let controls = 0;
  walk(ast.html, (node) => {
    if (node.type !== "Element" || (node.name !== "select" && node.name !== "input")) return;
    controls += 1;
    assert.ok(attribute(node, "disabled"), `${node.name} at offset ${node.start} has no disabled state`);
  });
  assert.ok(controls >= 8, `expected the launcher editable-control inventory, found only ${controls}`);
});

test("every frontend Tauri command is registered and its arguments match Rust", () => {
  const contracts = rustCommandContracts();
  const registered = registeredCommands();
  const invocations = frontendInvocations();
  assert.ok(invocations.length >= 20, `expected the launcher invoke inventory, found only ${invocations.length}`);

  for (const invocation of invocations) {
    assert.ok(registered.has(invocation.command), `App.svelte:${invocation.line} invokes unregistered command ${invocation.command}`);
    assert.ok(contracts.has(invocation.command), `App.svelte:${invocation.line} invokes command without a #[tauri::command] contract: ${invocation.command}`);
    assert.deepEqual(
      invocation.keys,
      contracts.get(invocation.command),
      `App.svelte:${invocation.line} sends the wrong arguments to ${invocation.command}`,
    );
  }

  const invokedCommands = new Set(invocations.map((entry) => entry.command));
  assert.deepEqual(
    [...registered].sort(),
    [...invokedCommands].sort(),
    "Every registered Tauri command must have a frontend caller",
  );
});

test("hardcoded external links stay on the official HTTPS repository", () => {
  const urls = [];
  walk(ast.html, (node) => {
    if (node.type !== "CallExpression" || node.callee?.type !== "Identifier" || node.callee.name !== "openUpdateUrl") return;
    const argument = node.arguments?.[0];
    if (argument?.type === "Literal" && typeof argument.value === "string") urls.push(argument.value);
  });
  assert.ok(urls.length >= 3, `expected official repository links, found only ${urls.length}`);
  for (const value of urls) {
    const url = new URL(value);
    assert.equal(url.protocol, "https:", `external launcher link must use HTTPS: ${value}`);
    assert.equal(url.hostname, "github.com", `external launcher link must use github.com: ${value}`);
    assert.ok(url.pathname.startsWith("/OAleex/MojoRecomp"), `external launcher link left the official repository: ${value}`);
  }
});

test("launcher navigation and game update badges stay in separate scopes", () => {
  const functions = functionDeclarations();
  const openLauncher = functions.get("openLauncher");
  const checkLauncher = functions.get("checkLauncherUpdates");
  const gameAttention = functions.get("gameNeedsAttention");
  const componentAttention = functions.get("componentNeedsAttention");
  assert.ok(openLauncher, "openLauncher handler is required");
  assert.ok(checkLauncher, "checkLauncherUpdates handler is required");
  assert.ok(gameAttention, "gameNeedsAttention helper is required");
  assert.ok(componentAttention, "componentNeedsAttention helper is required");

  const openLauncherSource = appSource.slice(openLauncher.start, openLauncher.end);
  const checkLauncherSource = appSource.slice(checkLauncher.start, checkLauncher.end);
  const gameAttentionSource = appSource.slice(gameAttention.start, gameAttention.end);
  const componentAttentionSource = appSource.slice(componentAttention.start, componentAttention.end);

  assert.match(openLauncherSource, /activeView\s*=\s*"launcher"/, "Mojo button must open the Launcher page");
  assert.doesNotMatch(openLauncherSource, /checkUpdates|check_launcher_update|invoke\(/, "Opening the Launcher page must not perform an update check");
  assert.match(checkLauncherSource, /"check_launcher_update"/, "Launcher checks must use the launcher-only Tauri command");
  assert.doesNotMatch(gameAttentionSource, /\.playable/, "Game badges must not disappear only because the game is not yet playable");
  assert.match(componentAttentionSource, /componentHasDownload\(component\)/, "Missing runtimes should alert only when a downloadable release exists");
  assert.match(appSource, /class="versions-tab-badge"/, "Versions tab must expose the game update badge");
  assert.match(appSource, /aria-label=\{gameNeedsAttention\(game\.id\)/, "Game rail must announce pending runtime updates");
  assert.match(appSource, /aria-label=\{versionsNeedsAttention \? "Versions, update available"/, "Versions tab must announce pending updates");
  assert.match(appSource, /<h1>Help<\/h1>/, "Help page title must be Help");
  assert.doesNotMatch(appSource, /Support &amp; FAQ|Support & FAQ/, "Legacy Support & FAQ title must not return");

  const runtimeCheckMatch = rustSource.match(/async fn check_component_updates\([\s\S]*?\n\}/);
  assert.ok(runtimeCheckMatch, "check_component_updates command is required");
  assert.match(
    runtimeCheckMatch[0],
    /component\.id != "launcher"/,
    "Runtime/component update checks must exclude the launcher",
  );
});

test("support package minidump remains explicit opt-in", () => {
  const createSupport = functionDeclarations().get("createSupportArchive");
  assert.ok(createSupport, "createSupportArchive handler is required");
  const source = appSource.slice(createSupport.start, createSupport.end);
  assert.match(source, /includeMinidump:\s*includeSupportMinidump/, "Support package must pass the explicit minidump consent state");
  assert.match(appSource, /let includeSupportMinidump = false;/, "Minidump consent must default to off");
  assert.match(appSource, /Include memory diagnostic \(\.dmp\)/, "Help must explain the optional minidump");
  assert.match(appSource, /never uploaded automatically/, "Help must state that minidumps are not uploaded automatically");
});

test("game library setup only presents migration progress when the backend reports a real move", () => {
  const applyLibrary = functionDeclarations().get("applyLibraryLocation");
  assert.ok(applyLibrary, "applyLibraryLocation handler is required");
  const source = appSource.slice(applyLibrary.start, applyLibrary.end);

  assert.match(source, /libraryProgress\s*=\s*null/, "starting a library change must clear stale migration progress");
  assert.doesNotMatch(
    source,
    /libraryProgress\s*=\s*\{[\s\S]*?stage:\s*"planning"/,
    "the UI must not invent migration progress before the backend finds data to move",
  );
  assert.match(
    appSource,
    /libraryProgress\?\.stage === "moving" \? "Moving\.\.\."/,
    "Moving must be shown only for an actual moving stage",
  );
  assert.match(appSource, /firstLaunchPending \? "Setting up\.\.\." : "Applying\.\.\."/, "empty first-run setup must use a non-migration label");
});

test("optional languages stay out of normal language selectors until installed", () => {
  const buildLanguages = functionDeclarations().get("buildLanguageOptions");
  assert.ok(buildLanguages, "buildLanguageOptions helper is required");
  const source = appSource.slice(buildLanguages.start, buildLanguages.end);
  assert.match(
    source,
    /if \(!component\.installed_version\) continue;/,
    "published-but-uninstalled language components must not appear as selectable game languages",
  );
  assert.match(appSource, /<h2 id="additional-content-title">Additional content<\/h2>/, "runtime installs must expose the additional-content chooser");
  assert.match(appSource, /invoke<RuntimeAdditionalContentStatus \| null>\("runtime_additional_content"/, "runtime chooser must load content tied to the selected runtime release");
  assert.match(appSource, /"install_runtime_additional_content"/, "selected runtime extras must use the single-pack installation command");
});
