let appConfig = null;
let editingRuleId = null;
let editingGroupId = null;
let editingGroupApps = [];
let networkOptions = [];
const shortcutPreviews = new Map();
const appPathStatus = new Map();

const $ = (id) => document.getElementById(id);

function newId(prefix) {
  return `${prefix}_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 7)}`;
}

function newRuleId() {
  return newId("rule");
}

function newGroupId() {
  return newId("group");
}

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#039;"
  })[char]);
}

function cloneApp(app) {
  return {
    name: app.name || app.path || app.original_path || "",
    original_path: app.original_path || app.path || "",
    icon_path: app.icon_path || app.original_path || app.path || "",
    shortcut_paths: [...(app.shortcut_paths || [])],
    replaced_shortcuts: (app.replaced_shortcuts || []).map((shortcut) => ({ ...shortcut }))
  };
}

function appsSamePath(a, b) {
  return String(a || "").toLowerCase() === String(b || "").toLowerCase();
}

function networkKey(type, id) {
  return `${type || "wifi"}:${id || ""}`;
}

function normalizeNetworkRule(rule) {
  rule.network_type ||= "wifi";
  rule.network_id ||= rule.ssid || "";
  rule.network_name ||= rule.network_id || rule.ssid || "";
  if (rule.network_type === "wifi" && !rule.ssid) rule.ssid = rule.network_id;
}

function displayNetwork(ruleOrOption) {
  const type = ruleOrOption.network_type || ruleOrOption.type || "wifi";
  const name = ruleOrOption.network_name || ruleOrOption.name || ruleOrOption.network_id || ruleOrOption.id || ruleOrOption.ssid || "";
  const label = type === "wired" ? t("wired") : t("wifi");
  return name ? `${label}: ${name}` : t("notSelected");
}

function setTab(tab) {
  document.querySelectorAll(".nav-item").forEach((button) => {
    button.classList.toggle("active", button.dataset.tab === tab);
  });
  document.querySelectorAll(".panel").forEach((panel) => panel.classList.remove("active"));
  $(`${tab}Panel`).classList.add("active");
  if (tab === "stats") loadStats();
}

function normalizeConfig() {
  appConfig.settings ||= {};
  appConfig.rules ||= [];
  appConfig.app_groups ||= [];
  appConfig.version = "1.5.0";
  appConfig.settings.language ||= "zh-CN";
  appConfig.settings.bypass_timeout_minutes = 0;
  appConfig.settings.bypass_until_epoch = 0;

  const knownGroups = new Set(appConfig.app_groups.map((group) => group.id));
  for (const rule of appConfig.rules) {
    normalizeNetworkRule(rule);
    rule.blocked_apps ||= [];
    if (rule.app_group_id && knownGroups.has(rule.app_group_id)) continue;
    if (!rule.blocked_apps.length) continue;
    const generatedId = `legacy_${rule.id || newGroupId()}`;
    const generatedName = t("oldRuleGroupName", { name: rule.network_name || rule.ssid || t("oldRule") });
    appConfig.app_groups.push({
      id: generatedId,
      name: generatedName,
      apps: rule.blocked_apps.map((app) => {
        const copied = cloneApp(app);
        copied.replaced_shortcuts = [];
        return copied;
      })
    });
    rule.app_group_id = generatedId;
    knownGroups.add(generatedId);
  }
}

function appStatusKey(ruleId, appPath) {
  return `${ruleId}\n${String(appPath || "").toLowerCase()}`;
}

function statusForApp(ruleId, app) {
  return appPathStatus.get(appStatusKey(ruleId, app.original_path));
}

function groupById(groupId) {
  return (appConfig.app_groups || []).find((group) => group.id === groupId);
}

function groupName(groupId) {
  return groupById(groupId)?.name || t("selectGroup");
}

function shortcutDisplayName(path) {
  const fileName = String(path || "").split(/[\\/]/).pop() || "";
  return fileName.replace(/\.lnk$/i, "");
}

function appFromShortcut(shortcut) {
  const target = shortcut.target_path || "";
  const name = shortcutDisplayName(shortcut.path) || (target.split(/[\\/]/).pop() || target).replace(/\.exe$/i, "");
  return {
    name,
    original_path: target,
    icon_path: shortcut.icon_path || target,
    shortcut_paths: shortcut.path ? [shortcut.path] : [],
    replaced_shortcuts: []
  };
}

function mergeShortcutApps(existingApps, shortcuts) {
  let apps = (existingApps || []).map(cloneApp);
  for (const shortcut of shortcuts || []) {
    const shortcutPath = shortcut.path || "";
    const target = shortcut.target_path || "";
    if (!shortcutPath || !target) continue;
    const existing = apps.find((app) => appsSamePath(app.original_path, target));
    if (existing) {
      existing.name ||= shortcutDisplayName(shortcutPath);
      existing.icon_path ||= shortcut.icon_path || target;
      existing.shortcut_paths ||= [];
      if (!existing.shortcut_paths.some((path) => appsSamePath(path, shortcutPath))) {
        existing.shortcut_paths.push(shortcutPath);
      }
    } else {
      apps.push(appFromShortcut(shortcut));
    }
  }
  return apps;
}

function mergeAppsWithExistingReplacements(ruleId, apps) {
  const existingRule = appConfig.rules.find((item) => item.id === ruleId);
  if (!existingRule) return apps.map(cloneApp);
  return apps.map((source) => {
    const app = cloneApp(source);
    const previous = (existingRule.blocked_apps || []).find((item) => appsSamePath(item.original_path, app.original_path));
    app.replaced_shortcuts = previous?.replaced_shortcuts || [];
    return app;
  });
}

function renderGroupOptions() {
  const select = $("ruleGroupSelect");
  if (!select) return;
  select.innerHTML = `<option value="">${escapeHtml(t("selectGroup"))}</option>` + (appConfig.app_groups || [])
    .map((group) => `<option value="${escapeHtml(group.id)}">${escapeHtml(group.name)} (${(group.apps || []).length})</option>`)
    .join("");
}

function restoreCounts(result) {
  const items = result?.results || [];
  return {
    ok: items.filter((item) => item.ok).length,
    failed: items.filter((item) => !item.ok).length
  };
}

function restoreMessage(counts) {
  return counts.failed ? t("restoredMixed", counts) : t("restoredOk", counts);
}

function renderGroupAppRows(group) {
  const apps = editingGroupId === group?.id ? editingGroupApps : (group?.apps || []);
  if (!apps.length) return `<div class="empty-line">${escapeHtml(t("noGroupApps"))}</div>`;
  return apps.map((app) => `
    <div class="software-row">
      <div class="software-icon">LNK</div>
      <div>
        <strong>${escapeHtml(app.name || app.original_path)}</strong>
        <span>${escapeHtml(app.original_path)}</span>
        ${(app.shortcut_paths || []).map((path) => `<small>${escapeHtml(path)}</small>`).join("")}
      </div>
      ${editingGroupId === group.id ? `<button class="icon-button danger-text" type="button" data-remove-app="${escapeHtml(app.original_path)}" title="${escapeHtml(t("remove"))}">×</button>` : ""}
    </div>
  `).join("");
}

function renderGroups() {
  const list = $("appGroupsList");
  list.innerHTML = "";
  if (!appConfig.app_groups.length) {
    list.innerHTML = `<div class="empty-line">${escapeHtml(t("noGroups"))}</div>`;
  } else {
    for (const group of appConfig.app_groups) {
      const card = document.createElement("article");
      card.className = `software-group-card ${editingGroupId === group.id ? "editing" : ""}`;
      card.innerHTML = `
        <header>
          <div>
            <h3>${escapeHtml(group.name)}</h3>
            <p>${escapeHtml(t("appCount", { count: (group.apps || []).length }))}</p>
          </div>
          <div class="inline-actions">
            <button class="ghost-button" data-action="edit-group">${escapeHtml(t("edit"))}</button>
            <button class="ghost-button danger-text" data-action="delete-group">${escapeHtml(t("delete"))}</button>
          </div>
        </header>
        <div class="software-list compact">${renderGroupAppRows(group)}</div>
      `;
      card.querySelector('[data-action="edit-group"]').addEventListener("click", () => startGroupEdit(group.id));
      card.querySelector('[data-action="delete-group"]').addEventListener("click", () => deleteGroup(group.id));
      card.querySelectorAll("[data-remove-app]").forEach((button) => {
        button.addEventListener("click", () => {
          editingGroupApps = editingGroupApps.filter((app) => !appsSamePath(app.original_path, button.dataset.removeApp));
          renderEditingShortcutList();
          renderGroups();
        });
      });
      list.appendChild(card);
    }
  }
  renderGroupOptions();
  renderRuleSummary();
}

function resetGroupForm() {
  editingGroupId = null;
  editingGroupApps = [];
  $("groupName").value = "";
  $("groupFormTitle").textContent = t("newGroup");
  $("groupForm").classList.add("hidden");
  renderEditingShortcutList();
  renderGroups();
}

function startGroupEdit(groupId = "") {
  const group = groupId ? groupById(groupId) : null;
  editingGroupId = group?.id || "";
  editingGroupApps = (group?.apps || []).map(cloneApp);
  $("groupFormTitle").textContent = group ? t("editGroup") : t("newGroup");
  $("groupName").value = group?.name || "";
  $("groupForm").classList.remove("hidden");
  renderEditingShortcutList();
  $("groupName").focus();
  renderGroups();
}

async function deleteGroup(groupId) {
  const group = groupById(groupId);
  if (!group) return;
  const usedRules = appConfig.rules.filter((rule) => rule.app_group_id === groupId);
  const detail = usedRules.length ? `\n${t("deleteGroupUsed", { count: usedRules.length })}` : "";
  if (!confirm(`${t("deleteGroupConfirm", { name: group.name })}${detail}`)) return;

  for (const rule of usedRules) {
    if ((rule.blocked_apps || []).some((app) => (app.replaced_shortcuts || []).length)) {
      const restored = await Api.restoreShortcuts(rule.id);
      appConfig = restored.config || appConfig;
      normalizeConfig();
      const counts = restoreCounts(restored);
      if (counts.failed) {
        applySettings();
        renderGroups();
        renderRules();
        await loadAppStatuses();
        toast(t("restoredMixed", counts));
        return;
      }
    }
  }

  appConfig.rules = appConfig.rules.filter((rule) => rule.app_group_id !== groupId);
  appConfig.app_groups = appConfig.app_groups.filter((item) => item.id !== groupId);
  if (editingGroupId === groupId) resetGroupForm();
  await saveConfig(t("groupDeleted"));
}

function renderEditingShortcutList() {
  const list = $("groupShortcutList");
  if (!list) return;
  if (!editingGroupApps.length) {
    list.innerHTML = `<div class="empty-line">${escapeHtml(t("noEditingShortcuts"))}</div>`;
    return;
  }
  list.innerHTML = editingGroupApps.map((app) => `
    <div class="software-row">
      <div class="software-icon">LNK</div>
      <div>
        <strong>${escapeHtml(app.name || app.original_path)}</strong>
        <span>${escapeHtml(app.original_path)}</span>
        ${(app.shortcut_paths || []).map((path) => `<small>${escapeHtml(path)}</small>`).join("")}
      </div>
      <button class="icon-button danger-text" type="button" data-remove-editing-app="${escapeHtml(app.original_path)}" title="${escapeHtml(t("remove"))}">×</button>
    </div>
  `).join("");
  list.querySelectorAll("[data-remove-editing-app]").forEach((button) => {
    button.addEventListener("click", () => {
      editingGroupApps = editingGroupApps.filter((app) => !appsSamePath(app.original_path, button.dataset.removeEditingApp));
      renderEditingShortcutList();
      renderGroups();
    });
  });
}

async function saveGroupFromForm(event) {
  event.preventDefault();
  const name = $("groupName").value.trim();
  const apps = editingGroupApps.map(cloneApp);
  if (!name) {
    toast(t("groupNameRequired"));
    return;
  }
  if (!apps.length) {
    toast(t("groupAppsRequired"));
    return;
  }

  const group = {
    id: editingGroupId || newGroupId(),
    name,
    apps
  };
  const usedRuleIds = appConfig.rules
    .filter((rule) => rule.app_group_id === group.id)
    .map((rule) => rule.id);
  const index = appConfig.app_groups.findIndex((item) => item.id === group.id);
  if (index >= 0) appConfig.app_groups[index] = group;
  else appConfig.app_groups.push(group);

  for (const rule of appConfig.rules) {
    if (rule.app_group_id !== group.id) continue;
    rule.blocked_apps = mergeAppsWithExistingReplacements(rule.id, group.apps);
  }

  await saveConfig(t("groupSaved"));
  resetGroupForm();
  for (const ruleId of usedRuleIds) {
    await replaceRuleShortcuts(ruleId);
  }
}

function appendGroupShortcuts(shortcuts) {
  editingGroupApps = mergeShortcutApps(editingGroupApps, shortcuts);
  renderEditingShortcutList();
  renderGroups();
}

function renderRuleApps(rule) {
  const apps = rule.blocked_apps || [];
  if (!apps.length) return `<div class="app-status-list muted">${escapeHtml(t("none"))}</div>`;
  const rows = apps.map((app) => {
    const status = statusForApp(rule.id, app);
    const missing = status?.missing === true;
    const label = missing ? t("missing") : (status ? t("ready") : t("checking"));
    return `
      <div class="app-status-row ${missing ? "missing" : ""}">
        <div>
          <strong>${escapeHtml(app.name || app.original_path)}</strong>
          <span>${escapeHtml(app.original_path)}</span>
        </div>
        <span class="app-status-badge">${escapeHtml(label)}</span>
        ${missing ? `<button class="ghost-button danger-text" data-action="cleanup-app" data-app-path="${escapeHtml(app.original_path)}">${escapeHtml(t("cleanup"))}</button>` : ""}
      </div>
    `;
  }).join("");
  return `<div class="app-status-list">${rows}</div>`;
}

function renderShortcutPreview(ruleId) {
  const preview = shortcutPreviews.get(ruleId);
  if (!preview) return "";
  if (preview.loading) return `<div class="shortcut-preview muted">${escapeHtml(t("scanning"))}</div>`;

  const rows = (preview.items || []).map((item) => `
    <div class="shortcut-row ${item.ok === false ? "failed" : ""}">
      <strong>${escapeHtml(item.app || item.path || "")}</strong>
      <span>${escapeHtml(item.message || item.path || "")}</span>
    </div>
  `).join("");

  return `
    <div class="shortcut-preview" data-preview-for="${escapeHtml(ruleId)}">
      <div class="preview-title">${escapeHtml(preview.message || t("scanDone", { count: preview.count || 0 }))}</div>
      ${rows || `<div class="shortcut-row muted">${escapeHtml(t("noShortcutFound"))}</div>`}
    </div>
  `;
}

function renderRules() {
  const list = $("rulesList");
  list.innerHTML = "";
  if (!appConfig.rules.length) {
    list.innerHTML = `<div class="settings-card muted">${escapeHtml(t("noRules"))}</div>`;
    renderRuleSummary();
    return;
  }

  for (const rule of appConfig.rules) {
    normalizeNetworkRule(rule);
    const card = document.createElement("article");
    card.className = "rule-card";
    const shortcutCount = (rule.blocked_apps || []).reduce((sum, app) => sum + (app.replaced_shortcuts || []).length, 0);
    card.innerHTML = `
      <header>
        <div>
          <h2>${escapeHtml(displayNetwork(rule))}</h2>
          <div class="pill-row">
            <span class="pill">${escapeHtml(t("softwareGroup"))}: ${escapeHtml(groupName(rule.app_group_id))}</span>
            <span class="pill">${escapeHtml(t("appCount", { count: (rule.blocked_apps || []).length }))}</span>
            <span class="pill">${escapeHtml(t("replacedShortcuts", { count: shortcutCount }))}</span>
          </div>
        </div>
      </header>
      <p class="muted">${escapeHtml(t("ruleMuted"))}</p>
      ${renderRuleApps(rule)}
      ${renderShortcutPreview(rule.id)}
      <div class="rule-actions">
        <button class="ghost-button" data-action="edit">${escapeHtml(t("edit"))}</button>
        <button class="ghost-button" data-action="scan">${escapeHtml(t("scanShortcuts"))}</button>
        <button class="ghost-button" data-action="replace">${escapeHtml(t("replaceAgain"))}</button>
        <button class="ghost-button danger-text" data-action="delete">${escapeHtml(t("delete"))}</button>
      </div>
    `;
    card.querySelector('[data-action="edit"]').addEventListener("click", () => startRuleEdit(rule.id));
    card.querySelector('[data-action="scan"]').addEventListener("click", () => scanRuleShortcuts(rule.id));
    card.querySelector('[data-action="replace"]').addEventListener("click", () => replaceRuleShortcuts(rule.id));
    card.querySelector('[data-action="delete"]').addEventListener("click", () => deleteRule(rule.id));
    card.querySelectorAll('[data-action="cleanup-app"]').forEach((button) => {
      button.addEventListener("click", () => cleanupMissingApp(rule.id, button.dataset.appPath));
    });
    list.appendChild(card);
  }
  renderRuleSummary();
}

function selectedNetwork() {
  const manual = $("manualNetwork").value.trim();
  if (manual) {
    const matched = networkOptions.find((item) => {
      return String(item.id || "").toLowerCase() === manual.toLowerCase()
        || String(item.name || "").toLowerCase() === manual.toLowerCase();
    });
    if (matched) return matched;
    return {
      type: "wifi",
      id: manual,
      name: manual,
      manual: true
    };
  }
  const selected = $("ruleNetworkSelect").value;
  return networkOptions.find((item) => item.key === selected) || null;
}

function resetRuleForm() {
  editingRuleId = null;
  $("ruleNetworkSelect").value = "";
  $("manualNetwork").value = "";
  $("ruleGroupSelect").value = "";
  $("ruleFormTitle").textContent = t("addRule");
  $("saveRuleButton").textContent = t("saveRule");
  renderRuleSummary();
}

function startRuleEdit(ruleId) {
  const rule = appConfig.rules.find((item) => item.id === ruleId);
  if (!rule) return;
  normalizeNetworkRule(rule);
  editingRuleId = rule.id;
  $("ruleFormTitle").textContent = t("editRule");
  $("saveRuleButton").textContent = t("saveEditedRule");
  const key = networkKey(rule.network_type, rule.network_id);
  const optionExists = networkOptions.some((option) => option.key === key);
  $("ruleNetworkSelect").value = optionExists ? key : "";
  $("manualNetwork").value = optionExists ? "" : (rule.network_name || rule.network_id || rule.ssid);
  $("ruleGroupSelect").value = rule.app_group_id || "";
  $("ruleNetworkSelect").focus();
  renderRuleSummary();
}

function renderRuleSummary() {
  const network = selectedNetwork();
  const group = groupById($("ruleGroupSelect")?.value || "");
  $("summaryNetwork").textContent = network ? displayNetwork(network) : t("notSelected");
  $("summaryGroup").textContent = group ? t("groupWithCount", { name: group.name, count: (group.apps || []).length }) : t("notSelected");
  $("summaryAction").textContent = network && group ? t("summaryReady") : t("summaryWaiting");
}

async function saveRuleFromForm(event) {
  event.preventDefault();
  const network = selectedNetwork();
  const groupId = $("ruleGroupSelect").value;
  const group = groupById(groupId);
  if (!network || !network.id) {
    toast(t("chooseNetworkFirst"));
    return;
  }
  if (!group || !(group.apps || []).length) {
    toast(t("chooseGroupFirst"));
    return;
  }

  const ruleId = editingRuleId || newRuleId();
  const existingRule = appConfig.rules.find((item) => item.id === ruleId);
  const rule = {
    id: ruleId,
    ssid: network.type === "wifi" ? network.id : "",
    network_type: network.type,
    network_id: network.id,
    network_name: network.name || network.id,
    app_group_id: group.id,
    blocked_apps: mergeAppsWithExistingReplacements(ruleId, group.apps),
    safe_wifi_ssid: existingRule?.safe_wifi_ssid || "",
    safe_wifi_password: existingRule?.safe_wifi_password || "",
    description: `${displayNetwork(network)} -> ${group.name}`
  };

  const index = appConfig.rules.findIndex((item) => item.id === rule.id);
  if (index >= 0) appConfig.rules[index] = { ...appConfig.rules[index], ...rule };
  else appConfig.rules.push(rule);

  appConfig.settings.protection_enabled = true;
  await saveConfig(t("ruleSaved"));
  resetRuleForm();
  await replaceRuleShortcuts(rule.id);
}

async function replaceRuleShortcuts(ruleId) {
  const rule = appConfig.rules.find((item) => item.id === ruleId);
  if (!rule) return;
  let total = 0;
  const resultItems = [];
  for (const app of rule.blocked_apps || []) {
    const result = await Api.replaceShortcuts(app.original_path, rule.id);
    appConfig = result.config || appConfig;
    normalizeConfig();
    for (const item of result.results || []) {
      if (item.ok) total += 1;
      resultItems.push({
        app: app.name || app.original_path,
        ok: item.ok,
        path: item.shortcut,
        message: item.ok ? item.shortcut : (item.error || item.shortcut || "Failed")
      });
    }
  }
  shortcutPreviews.set(ruleId, {
    count: total,
    message: t("replaceDone", { count: total }),
    items: resultItems
  });
  applySettings();
  renderGroups();
  renderRules();
  loadAppStatuses().catch(() => {});
  toast(t("replaceDone", { count: total }));
}

async function deleteRule(ruleId) {
  const rule = appConfig.rules.find((item) => item.id === ruleId);
  if (!rule) return;
  if (!confirm(t("deleteRuleConfirm", { name: displayNetwork(rule) }))) return;
  if ((rule.blocked_apps || []).some((app) => (app.replaced_shortcuts || []).length)) {
    const restored = await Api.restoreShortcuts(ruleId);
    appConfig = restored.config || appConfig;
    normalizeConfig();
    const counts = restoreCounts(restored);
    if (counts.failed) {
      applySettings();
      renderGroups();
      renderRules();
      await loadAppStatuses();
      toast(t("restoredMixed", counts));
      return;
    }
  }
  appConfig.rules = appConfig.rules.filter((item) => item.id !== ruleId);
  shortcutPreviews.delete(ruleId);
  await saveConfig(t("ruleDeleted"));
}

async function restoreAllShortcutsFromUi() {
  if (!confirm(t("shortcutRestoreConfirm"))) return;
  try {
    const result = await Api.restoreShortcuts();
    appConfig = result.config || appConfig;
    normalizeConfig();
    shortcutPreviews.clear();
    applySettings();
    renderGroups();
    renderRules();
    await loadAppStatuses();
    toast(restoreMessage(restoreCounts(result)));
  } catch (error) {
    toast(error.message);
  }
}

async function loadAppStatuses() {
  const result = await Api.appStatus();
  appPathStatus.clear();
  for (const rule of result.rules || []) {
    for (const app of rule.apps || []) {
      appPathStatus.set(appStatusKey(rule.rule_id, app.path), app);
    }
  }
  renderRules();
}

async function cleanupMissingApp(ruleId, appPath) {
  if (!appPath) return;
  try {
    const result = await Api.cleanupApp(ruleId, appPath);
    appConfig = result.config || appConfig;
    normalizeConfig();
    const rule = appConfig.rules.find((item) => item.id === ruleId);
    const group = rule ? groupById(rule.app_group_id) : null;
    if (group) group.apps = (group.apps || []).filter((app) => !appsSamePath(app.original_path, appPath));
    await loadAppStatuses();
    await saveConfig(t("toastSaved"));
    toast(t("toastSaved"));
  } catch (error) {
    toast(error.message);
  }
}

async function scanRuleShortcuts(ruleId) {
  const rule = appConfig.rules.find((item) => item.id === ruleId);
  if (!rule) return;
  shortcutPreviews.set(ruleId, { loading: true });
  renderRules();

  const items = [];
  for (const app of rule.blocked_apps || []) {
    const result = await Api.scanShortcuts(app.original_path);
    const shortcuts = result.shortcuts || [];
    if (!shortcuts.length) {
      items.push({
        app: app.name || app.original_path,
        message: t("noShortcutFound"),
        ok: true
      });
      continue;
    }
    for (const shortcut of shortcuts) {
      items.push({
        app: app.name || app.original_path,
        path: shortcut.path,
        message: shortcut.path,
        ok: true
      });
    }
  }

  const count = items.filter((item) => item.path).length;
  shortcutPreviews.set(ruleId, {
    count,
    message: t("scanDone", { count }),
    items
  });
  renderRules();
  toast(t("scanDone", { count }));
}

async function saveConfig(message = t("toastSaved")) {
  normalizeConfig();
  const result = await Api.saveConfig(appConfig);
  appConfig = result.config;
  normalizeConfig();
  applyLanguage(appConfig.settings.language);
  applySettings();
  renderGroups();
  renderRules();
  loadAppStatuses().catch(() => {});
  if (appConfig.settings.auto_start && result.auto_start_synced === false) {
    toast(result.auto_start_error || "Failed to update startup entry");
  } else {
    toast(message);
  }
}

function applySettings() {
  $("protectionToggle").checked = appConfig.settings.protection_enabled;
  $("autoStartToggle").checked = appConfig.settings.auto_start;
  $("darkToggle").checked = appConfig.settings.dark_mode;
  document.body.classList.toggle("dark", appConfig.settings.dark_mode);
}

function actionLabel(row) {
  const key = row.action || row.wifi_action || "";
  const labels = {
    blocked: t("actionBlocked"),
    bypassed: t("actionBypassed"),
    shortcut_restored: t("actionShortcutRestored"),
    shortcut_restore_failed: t("actionShortcutRestoreFailed"),
    shortcut_replace_failed: t("actionShortcutReplaceFailed"),
    config_repaired: t("actionConfigRepaired"),
    protection_disabled_no_adapter: t("actionNoAdapter"),
    switched_to: t("actionSwitched"),
    connect_requested: t("actionConnectRequested"),
    native_dialog_opened: t("actionDialogOpened"),
    switch_failed: t("actionSwitchFailed"),
    wired_disable_requested: t("actionWiredDisabled"),
    wired_enable_requested: t("actionWiredEnabled")
  };
  return labels[key] || key;
}

async function loadNetworkOptions() {
  const [wifiData, wiredData, currentData] = await Promise.all([
    Api.availableWifi().catch(() => ({ networks: [], known_profiles: [] })),
    Api.wiredAdapters().catch(() => ({ adapters: [] })),
    Api.currentNetwork().catch(() => ({}))
  ]);
  const options = [];
  const seen = new Set();
  const addOption = (option) => {
    if (!option.id) return;
    const key = networkKey(option.type, option.id);
    if (seen.has(key)) return;
    seen.add(key);
    options.push({ ...option, key });
  };

  for (const name of wifiData.known_profiles || []) {
    addOption({ type: "wifi", id: name, name });
  }
  for (const network of wifiData.networks || []) {
    addOption({
      type: "wifi",
      id: network.ssid,
      name: network.ssid,
      connected: network.connected
    });
  }
  for (const adapter of wiredData.adapters || []) {
    addOption({
      type: "wired",
      id: adapter.id,
      name: adapter.name || adapter.id,
      connected: adapter.connected,
      status: adapter.status
    });
  }
  if (currentData?.id) {
    addOption({
      type: currentData.type || "wifi",
      id: currentData.id,
      name: currentData.name || currentData.id,
      connected: currentData.connected
    });
  }

  networkOptions = options;
  $("networkOptions").innerHTML = options.map((item) => `<option value="${escapeHtml(item.name || item.id)}"></option>`).join("");
  $("ruleNetworkSelect").innerHTML = `<option value="">${escapeHtml(t("selectNetwork"))}</option>` + options
    .map((item) => `<option value="${escapeHtml(item.key)}">${escapeHtml(displayNetwork(item))}${item.connected ? ` (${escapeHtml(t("current"))})` : ""}</option>`)
    .join("");
  renderRuleSummary();
}

async function loadStats() {
  const data = await Api.stats();
  const stats = data.stats || {};
  $("blockedToday").textContent = stats.blocked_today || 0;
  $("blockedWeek").textContent = stats.blocked_week || 0;
  $("topApp").textContent = stats.top_blocked_app || t("none");
  $("logRows").innerHTML = (stats.records || []).slice(-80).reverse().map((row) => `
    <tr>
      <td>${escapeHtml(row.timestamp || "")}</td>
      <td>${escapeHtml(row.ssid || row.target_ssid || "")}</td>
      <td>${escapeHtml(row.app || row.shortcut || "")}</td>
      <td>${escapeHtml(actionLabel(row))}</td>
    </tr>
  `).join("");
}

function applyLanguage(lang) {
  I18N.setLanguage(lang);
  $("ruleFormTitle").textContent = editingRuleId ? t("editRule") : t("addRule");
  $("saveRuleButton").textContent = editingRuleId ? t("saveEditedRule") : t("saveRule");
  $("groupFormTitle").textContent = editingGroupId ? t("editGroup") : t("newGroup");
  renderGroupOptions();
  renderEditingShortcutList();
  renderGroups();
  renderRules();
  renderRuleSummary();
  loadNetworkOptions().catch(() => {});
  if ($("statsPanel").classList.contains("active")) loadStats().catch(() => {});
}

function networkStatusText(network) {
  if (network.adapter_available === false) return t("currentNoAdapter");
  if (!network.connected || !network.id) return t("currentNoNetwork");
  return `${t("connected")} ${displayNetwork({
    type: network.type,
    id: network.id,
    name: network.name || network.id
  })}`;
}

async function init() {
  const data = await Api.getConfig();
  appConfig = data.config;
  normalizeConfig();
  $("configPath").textContent = data.config_path || "";
  applyLanguage(currentLanguageFromConfig(appConfig));
  applySettings();
  renderGroups();
  renderRules();
  loadAppStatuses().catch(() => {});
  loadNetworkOptions().catch(() => {});

  if (location.hash === "#stats") setTab("stats");

  document.querySelectorAll(".nav-item").forEach((button) => {
    button.addEventListener("click", () => setTab(button.dataset.tab));
  });

  document.querySelectorAll("[data-lang]").forEach((button) => {
    button.addEventListener("click", async () => {
      const lang = button.dataset.lang;
      appConfig.settings.language = lang;
      applyLanguage(lang);
      await saveConfig(t("toastSaved"));
    });
  });

  $("newGroupButton").addEventListener("click", () => startGroupEdit(""));
  $("cancelGroupEdit").addEventListener("click", resetGroupForm);
  $("groupForm").addEventListener("submit", saveGroupFromForm);
  $("browseGroupShortcuts").addEventListener("click", async () => {
    try {
      const result = await Api.browseShortcuts();
      if (result.cancelled) return;
      const shortcuts = (result.shortcuts || []).filter((shortcut) => shortcut.target_path);
      appendGroupShortcuts(shortcuts);
      toast(t("selectedShortcuts", { count: shortcuts.length }));
    } catch (error) {
      toast(error.message);
    }
  });

  $("addRuleButton").addEventListener("click", resetRuleForm);
  $("restoreAllShortcutsButton").addEventListener("click", restoreAllShortcutsFromUi);
  $("ruleNetworkSelect").addEventListener("change", renderRuleSummary);
  $("manualNetwork").addEventListener("input", renderRuleSummary);
  $("ruleGroupSelect").addEventListener("change", renderRuleSummary);
  $("ruleForm").addEventListener("submit", saveRuleFromForm);

  $("protectionToggle").addEventListener("change", async (event) => {
    let message = event.target.checked ? t("protectionOn") : t("protectionOff");
    appConfig.settings.protection_enabled = event.target.checked;
    if (!event.target.checked) {
      const restored = await Api.restoreShortcuts();
      appConfig = restored.config || appConfig;
      normalizeConfig();
      appConfig.settings.protection_enabled = false;
      message = restoreMessage(restoreCounts(restored));
    }
    await saveConfig(message);
  });
  $("autoStartToggle").addEventListener("change", async (event) => {
    appConfig.settings.auto_start = event.target.checked;
    await saveConfig(t("autostartSaved"));
  });
  $("darkToggle").addEventListener("change", async (event) => {
    appConfig.settings.dark_mode = event.target.checked;
    await saveConfig(t("themeSaved"));
  });

  $("passwordForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    const password = $("bypassPassword").value;
    if (password) {
      const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(password));
      appConfig.settings.bypass_password = [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
    }
    appConfig.settings.bypass_timeout_minutes = 0;
    appConfig.settings.bypass_until_epoch = 0;
    $("bypassPassword").value = "";
    await saveConfig(t("passwordSaved"));
  });

  $("refreshStats").addEventListener("click", loadStats);
  startNetworkPolling((network) => {
    $("currentSsid").textContent = networkStatusText(network);
  }, 5000);
}

init().catch((error) => toast(error.message));
