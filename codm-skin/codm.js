'use strict';

// codm-skin — frida il2cpp recon + client-side skin id patcher.
// Client-side visuals only; server inventory untouched.
// frida is detected on live ACE sessions — emulator / offline accounts only.

const CONFIG_PATH = '/data/local/tmp/codm_config.json';
const DEFAULT_CONFIG = {
  mode: 'recon',
  filter: 'skin|camo|weapon|loadout|appearance',
  output: '/data/local/tmp/codm_recon.json',
  rules: [],
};

function log(msg) {
  console.log('[codm] ' + msg);
}

function loadConfig() {
  try {
    return JSON.parse(File.readAllText(CONFIG_PATH));
  } catch (e) {
    log('config not readable (' + e.message + '), defaults in use');
    return DEFAULT_CONFIG;
  }
}

function cstr(p) {
  return p.isNull() ? '' : p.readUtf8String();
}

// ---------------------------------------------------------------- il2cpp api

function buildApi() {
  const mod = Process.findModuleByName('libil2cpp.so');
  if (!mod) return null;

  const must = (name, ret, args) => {
    const p = mod.findExportByName(name);
    if (p === null) throw new Error('missing il2cpp export: ' + name);
    return new NativeFunction(p, ret, args);
  };
  const maybe = (name, ret, args) => {
    const p = mod.findExportByName(name);
    return p === null ? null : new NativeFunction(p, ret, args);
  };

  return {
    mod,
    domain_get: must('il2cpp_domain_get', 'pointer', []),
    domain_get_assemblies: must('il2cpp_domain_get_assemblies', 'pointer', ['pointer', 'pointer']),
    assembly_get_image: must('il2cpp_assembly_get_image', 'pointer', ['pointer']),
    image_get_name: must('il2cpp_image_get_name', 'pointer', ['pointer']),
    image_get_class_count: maybe('il2cpp_image_get_class_count', 'uint32', ['pointer']),
    image_get_class: maybe('il2cpp_image_get_class', 'pointer', ['pointer', 'uint32']),
    class_from_name: must('il2cpp_class_from_name', 'pointer', ['pointer', 'pointer', 'pointer']),
    class_get_name: must('il2cpp_class_get_name', 'pointer', ['pointer']),
    class_get_namespace: must('il2cpp_class_get_namespace', 'pointer', ['pointer']),
    class_get_methods: must('il2cpp_class_get_methods', 'pointer', ['pointer', 'pointer']),
    class_get_fields: maybe('il2cpp_class_get_fields', 'pointer', ['pointer', 'pointer']),
    method_get_name: must('il2cpp_method_get_name', 'pointer', ['pointer']),
    method_get_param_count: must('il2cpp_method_get_param_count', 'uint32', ['pointer']),
    method_get_return_type: must('il2cpp_method_get_return_type', 'pointer', ['pointer']),
    type_get_name: must('il2cpp_type_get_name', 'pointer', ['pointer']),
    field_get_name: maybe('il2cpp_field_get_name', 'pointer', ['pointer']),
    field_get_offset: maybe('il2cpp_field_get_offset', 'uint32', ['pointer']),
  };
}

let api = null;

function listImages() {
  const domain = api.domain_get();
  const countPtr = Memory.alloc(4);
  const assemblies = api.domain_get_assemblies(domain, countPtr);
  const count = countPtr.readU32();
  const out = [];
  for (let i = 0; i < count; i++) {
    const asm = assemblies.add(i * Process.pointerSize).readPointer();
    const img = api.assembly_get_image(asm);
    if (img.isNull()) continue;
    out.push({ ptr: img, name: cstr(api.image_get_name(img)) });
  }
  return out;
}

function classMethods(klass, limit) {
  const iter = Memory.alloc(Process.pointerSize);
  iter.writePointer(NULL);
  const out = [];
  let m;
  let guard = 0;
  while (!(m = api.class_get_methods(klass, iter)).isNull()) {
    if (++guard > 5000) break;
    const params = api.method_get_param_count(m);
    out.push({
      name: cstr(api.method_get_name(m)),
      params,
      returns: cstr(api.type_get_name(api.method_get_return_type(m))),
      address: m.readPointer(),
    });
    if (out.length >= limit) break;
  }
  return out;
}

function classFields(klass, limit) {
  if (!api.class_get_fields || !api.field_get_name || !api.field_get_offset) return [];
  const iter = Memory.alloc(Process.pointerSize);
  iter.writePointer(NULL);
  const out = [];
  let f;
  let guard = 0;
  while (!(f = api.class_get_fields(klass, iter)).isNull()) {
    if (++guard > 5000) break;
    out.push({ name: cstr(api.field_get_name(f)), offset: api.field_get_offset(f) });
    if (out.length >= limit) break;
  }
  return out;
}

// ---------------------------------------------------------------- recon mode

function recon(cfg) {
  const re = new RegExp(cfg.filter, 'i');
  const report = { generated: Date.now(), images: [], matches: [] };
  const images = listImages();
  for (const img of images) report.images.push(img.name);

  if (!api.image_get_class_count || !api.image_get_class) {
    log('image class enumeration unavailable on this build');
    File.writeAllText(cfg.output, JSON.stringify(report, null, 2));
    return;
  }

  for (const img of images) {
    const count = api.image_get_class_count(img.ptr);
    for (let i = 0; i < count; i++) {
      const klass = api.image_get_class(img.ptr, i);
      if (klass.isNull()) continue;
      const name = cstr(api.class_get_name(klass));
      const ns = cstr(api.class_get_namespace(klass));
      const full = ns ? ns + '.' + name : name;
      if (!re.test(full)) continue;
      report.matches.push({
        image: img.name,
        klass: full,
        methods: classMethods(klass, 200),
        fields: classFields(klass, 200),
      });
      if (report.matches.length >= 400) break;
    }
    if (report.matches.length >= 400) break;
  }

  File.writeAllText(cfg.output, JSON.stringify(report, null, 2));
  log('recon done: ' + report.matches.length + ' classes -> ' + cfg.output);
}

// ---------------------------------------------------------------- patch mode

function findClass(fullName) {
  let ns = '';
  let name = fullName;
  const dot = fullName.lastIndexOf('.');
  if (dot >= 0) {
    ns = fullName.substring(0, dot);
    name = fullName.substring(dot + 1);
  }
  const nsPtr = Memory.allocUtf8String(ns);
  const namePtr = Memory.allocUtf8String(name);
  for (const img of listImages()) {
    const k = api.class_from_name(img.ptr, nsPtr, namePtr);
    if (!k.isNull()) return k;
  }
  return null;
}

function findMethod(klass, methodName, paramCount) {
  const iter = Memory.alloc(Process.pointerSize);
  iter.writePointer(NULL);
  let m;
  let guard = 0;
  while (!(m = api.class_get_methods(klass, iter)).isNull()) {
    if (++guard > 5000) break;
    if (cstr(api.method_get_name(m)) !== methodName) continue;
    if (paramCount !== undefined && api.method_get_param_count(m) !== paramCount) continue;
    return m;
  }
  return null;
}

function resolveRule(rule) {
  if (rule.offset) {
    const rva = Number(rule.offset);
    if (!Number.isFinite(rva) || rva <= 0) throw new Error('bad offset: ' + rule.offset);
    return api.mod.base.add(rva);
  }
  const klass = findClass(rule.class);
  if (!klass) throw new Error('class not found: ' + rule.class);
  const method = findMethod(klass, rule.method, rule.params);
  if (!method) throw new Error('method not found: ' + rule.class + '::' + rule.method);
  const addr = method.readPointer();
  if (addr.isNull()) throw new Error('method has no pointer: ' + rule.method);
  return addr;
}

function patch(cfg) {
  const rules = Array.isArray(cfg.rules) ? cfg.rules : [];
  if (rules.length === 0) {
    log('no rules in config — nothing to patch');
    return;
  }
  let hooked = 0;
  for (const rule of rules) {
    try {
      const addr = resolveRule(rule);
      const value = rule.value;
      Interceptor.attach(addr, {
        onLeave(retval) {
          retval.replace(value);
        },
      });
      hooked++;
      log('hooked ' + (rule.offset ? rule.offset : rule.class + '::' + rule.method) + ' -> ' + value);
    } catch (e) {
      log('rule failed: ' + e.message);
    }
  }
  log(hooked + '/' + rules.length + ' rules live');
}

// ---------------------------------------------------------------- main

function waitForIl2cpp(callback) {
  const timer = setInterval(() => {
    if (!Process.findModuleByName('libil2cpp.so')) return;
    clearInterval(timer);
    try {
      api = buildApi();
      callback();
    } catch (e) {
      log('il2cpp init failed: ' + e.message);
    }
  }, 500);
}

function main() {
  const cfg = loadConfig();
  log('mode=' + cfg.mode);
  if (cfg.mode === 'recon') recon(cfg);
  else patch(cfg);
}

waitForIl2cpp(main);
