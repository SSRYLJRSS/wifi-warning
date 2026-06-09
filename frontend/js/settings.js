let appConfig = null;
let editingRuleId = null;
let editingGroupId = null;
let editingGroupApps = [];
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
  appConfig.settings.bypass_timeout_minutes = 0;
  appConfig.settings.bypass_until_epoch = 0;

  const knownGroups = new Set(appConfig.app_groups.map((group) => group.id));
  for (const rule of appConfig.rules) {
    rule.blocked_apps ||= [];
    if (rule.app_group_id && knownGroups.has(rule.app_group_id)) continue;
    if (!rule.blocked_apps.length) continue;
    const generatedId = `legacy_${rule.id || newGroupId()}`;
    const generatedName = `${rule.ssid || "旧规则"} 的软件`;
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
  return groupById(groupId)?.name || "未选择软件组";
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
  select.innerHTML = `<option value="">请选择软件组</option>` + (appConfig.app_groups || [])
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
  return counts.failed ? `已恢复 ${counts.ok} 个，${counts.failed} 个失败，可稍后重试` : `已恢复 ${counts.ok} 个快捷方式`;
}

function renderGroupAppRows(group) {
  const apps = editingGroupId === group?.id ? editingGroupApps : (group?.apps || []);
  if (!apps.length) return `<div class="empty-line">还没有添加软件。</div>`;
  return apps.map((app) => `
    <div class="software-row">
      <div class="software-icon">LNK</div>
      <div>
        <strong>${escapeHtml(app.name || app.original_path)}</strong>
        <span>${escapeHtml(app.original_path)}</span>
        ${(app.shortcut_paths || []).map((path) => `<small>${escapeHtml(path)}</small>`).join("")}
      </div>
      ${editingGroupId === group.id ? `<button class="icon-button danger-text" type="button" data-remove-app="${escapeHtml(app.original_path)}" title="移除">×</button>` : ""}
    </div>
  `).join("");
}

function renderGroups() {
  const list = $("appGroupsList");
  list.innerHTML = "";
  if (!appConfig.app_groups.length) {
    list.innerHTML = `<div class="empty-line">还没有软件组。先新建一个，例如“娱乐软件”或“工作时黑名单”。</div>`;
  } else {
    for (const group of appConfig.app_groups) {
      const card = document.createElement("article");
      card.className = `software-group-card ${editingGroupId === group.id ? "editing" : ""}`;
      card.innerHTML = `
        <header>
          <div>
            <h3>${escapeHtml(group.name)}</h3>
            <p>${(group.apps || []).length} 个软件</p>
          </div>
          <div class="inline-actions">
            <button class="ghost-button" data-action="edit-group">编辑</button>
            <button class="ghost-button danger-text" data-action="delete-group">删除</button>
          </div>
        </header>
        <div class="software-list compact">${renderGroupAppRows(group)}</div>
      `;
      card.querySelector('[data-action="edit-group"]').addEventListener("click", () => startGroupEdit(group.id));
      card.querySelector('[data-action="delete-group"]').addEventListener("click", () => deleteGroup(group.id));
      card.querySelectorAll('[data-remove-app]').forEach((button) => {
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
  $("groupFormTitle").textContent = "新建软件组";
  $("groupForm").classList.add("hidden");
  renderEditingShortcutList();
  renderGroups();
}

function startGroupEdit(groupId = "") {
  const group = groupId ? groupById(groupId) : null;
  editingGroupId = group?.id || "";
  editingGroupApps = (group?.apps || []).map(cloneApp);
  $("groupFormTitle").textContent = group ? "编辑软件组" : "新建软件组";
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
  const detail = usedRules.length ? `\n有 ${usedRules.length} 条规则正在使用它，删除后这些规则也会一起删除并尝试还原快捷方式。` : "";
  if (!confirm(`删除软件组“${group.name}”？${detail}`)) return;

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
        toast(`删除前有 ${counts.failed} 个快捷方式还原失败，已保留软件组`);
        return;
      }
    }
  }

  appConfig.rules = appConfig.rules.filter((rule) => rule.app_group_id !== groupId);
  appConfig.app_groups = appConfig.app_groups.filter((item) => item.id !== groupId);
  if (editingGroupId === groupId) resetGroupForm();
  await saveConfig("软件组已删除");
}

function renderEditingShortcutList() {
  const list = $("groupShortcutList");
  if (!list) return;
  if (!editingGroupApps.length) {
    list.innerHTML = `<div class="empty-line">还没有选择快捷方式。</div>`;
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
      <button class="icon-button danger-text" type="button" data-remove-editing-app="${escapeHtml(app.original_path)}" title="移除">×</button>
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
    toast("请填写软件组名称");
    return;
  }
  if (!apps.length) {
    toast("请至少选择一个快捷方式");
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

  await saveConfig("软件组已保存");
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
  if (!apps.length) return `<div class="app-status-list muted">暂无受限应用。</div>`;
  const rows = apps.map((app) => {
    const status = statusForApp(rule.id, app);
    const missing = status?.missing === true;
    const label = missing ? "已丢失" : (status ? "就绪" : "检查中");
    return `
      <div class="app-status-row ${missing ? "missing" : ""}">
        <div>
          <strong>${escapeHtml(app.name || app.original_path)}</strong>
          <span>${escapeHtml(app.original_path)}</span>
        </div>
        <span class="app-status-badge">${label}</span>
        ${missing ? `<button class="ghost-button danger-text" data-action="cleanup-app" data-app-path="${escapeHtml(app.original_path)}">清理</button>` : ""}
      </div>
    `;
  }).join("");
  return `<div class="app-status-list">${rows}</div>`;
}

function renderShortcutPreview(ruleId) {
  const preview = shortcutPreviews.get(ruleId);
  if (!preview) return "";
  if (preview.loading) return `<div class="shortcut-preview muted">正在扫描快捷方式...</div>`;

  const rows = (preview.items || []).map((item) => `
    <div class="shortcut-row ${item.ok === false ? "failed" : ""}">
      <strong>${escapeHtml(item.app || item.path || "")}</strong>
      <span>${escapeHtml(item.message || item.path || "")}</span>
    </div>
  `).join("");

  return `
    <div class="shortcut-preview" data-preview-for="${escapeHtml(ruleId)}">
      <div class="preview-title">${escapeHtml(preview.message || `发现 ${preview.count || 0} 个可替换快捷方式`)}</div>
      ${rows || `<div class="shortcut-row muted">未发现指向这些应用的快捷方式。</div>`}
    </div>
  `;
}

function renderRules() {
  const list = $("rulesList");
  list.innerHTML = "";
  if (!appConfig.rules.length) {
    list.innerHTML = `<div class="settings-card muted">暂无规则。按上面的三步选择 WiFi、软件组，然后确定。</div>`;
    renderRuleSummary();
    return;
  }

  for (const rule of appConfig.rules) {
    const card = document.createElement("article");
    card.className = "rule-card";
    const shortcutCount = (rule.blocked_apps || []).reduce((sum, app) => sum + (app.replaced_shortcuts || []).length, 0);
    card.innerHTML = `
      <header>
        <div>
          <h2>${escapeHtml(rule.ssid)}</h2>
          <div class="pill-row">
            <span class="pill">软件组: ${escapeHtml(groupName(rule.app_group_id))}</span>
            <span class="pill">${(rule.blocked_apps || []).length} 个软件</span>
            <span class="pill">已替换 ${shortcutCount} 个快捷方式</span>
          </div>
        </div>
      </header>
      <p class="muted">在这个 WiFi 下，点击这些软件的原快捷方式会先进入提醒页。</p>
      ${renderRuleApps(rule)}
      ${renderShortcutPreview(rule.id)}
      <div class="rule-actions">
        <button class="ghost-button" data-action="edit">编辑</button>
        <button class="ghost-button" data-action="scan">扫描快捷方式</button>
        <button class="ghost-button" data-action="replace">重新替换</button>
        <button class="ghost-button danger-text" data-action="delete">删除</button>
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

function resetRuleForm() {
  editingRuleId = null;
  $("ruleWifiSelect").value = "";
  $("manualSsid").value = "";
  $("ruleGroupSelect").value = "";
  $("ruleFormTitle").textContent = "添加规则";
  $("saveRuleButton").textContent = "确定规则并后台运行";
  renderRuleSummary();
}

function startRuleEdit(ruleId) {
  const rule = appConfig.rules.find((item) => item.id === ruleId);
  if (!rule) return;
  editingRuleId = rule.id;
  $("ruleFormTitle").textContent = "编辑规则";
  $("saveRuleButton").textContent = "保存规则并重新替换";
  const optionExists = Array.from($("ruleWifiSelect").options).some((option) => option.value === rule.ssid);
  $("ruleWifiSelect").value = optionExists ? rule.ssid : "";
  $("manualSsid").value = optionExists ? "" : rule.ssid;
  $("ruleGroupSelect").value = rule.app_group_id || "";
  $("ruleWifiSelect").focus();
  renderRuleSummary();
}

function selectedSsid() {
  return $("manualSsid").value.trim() || $("ruleWifiSelect").value.trim();
}

function renderRuleSummary() {
  const ssid = selectedSsid();
  const group = groupById($("ruleGroupSelect")?.value || "");
  $("summaryWifi").textContent = ssid || "未选择";
  $("summaryGroup").textContent = group ? `${group.name}（${(group.apps || []).length} 个软件）` : "未选择";
  $("summaryAction").textContent = ssid && group ? "确定后会替换软件组里选中的快捷方式，并保持后台防护。" : "完成前两步后即可确定。";
}

async function saveRuleFromForm(event) {
  event.preventDefault();
  const ssid = selectedSsid();
  const groupId = $("ruleGroupSelect").value;
  const group = groupById(groupId);
  if (!ssid) {
    toast("请先选择需要管控的 WiFi");
    return;
  }
  if (!group || !(group.apps || []).length) {
    toast("请先选择有软件的软件组");
    return;
  }

  const ruleId = editingRuleId || newRuleId();
  const existingRule = appConfig.rules.find((item) => item.id === ruleId);
  const rule = {
    id: ruleId,
    ssid,
    app_group_id: group.id,
    blocked_apps: mergeAppsWithExistingReplacements(ruleId, group.apps),
    safe_wifi_ssid: existingRule?.safe_wifi_ssid || "",
    safe_wifi_password: existingRule?.safe_wifi_password || "",
    description: `${ssid} 使用 ${group.name}`
  };

  const index = appConfig.rules.findIndex((item) => item.id === rule.id);
  if (index >= 0) appConfig.rules[index] = { ...appConfig.rules[index], ...rule };
  else appConfig.rules.push(rule);

  appConfig.settings.protection_enabled = true;
  await saveConfig("规则已保存，正在替换快捷方式");
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
        message: item.ok ? item.shortcut : (item.error || item.shortcut || "替换失败")
      });
    }
  }
  shortcutPreviews.set(ruleId, {
    count: total,
    message: `替换完成: 成功 ${total} 个`,
    items: resultItems
  });
  applySettings();
  renderGroups();
  renderRules();
  loadAppStatuses().catch(() => {});
  toast(`已替换 ${total} 个快捷方式`);
}

async function deleteRule(ruleId) {
  const rule = appConfig.rules.find((item) => item.id === ruleId);
  if (!rule) return;
  if (!confirm(`删除 ${rule.ssid} 的规则？已替换的快捷方式会先尝试还原。`)) return;
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
      toast(`删除前有 ${counts.failed} 个快捷方式还原失败，已保留规则`);
      return;
    }
  }
  appConfig.rules = appConfig.rules.filter((item) => item.id !== ruleId);
  shortcutPreviews.delete(ruleId);
  await saveConfig("规则已删除");
}

async function restoreAllShortcutsFromUi() {
  if (!confirm("恢复所有已替换的快捷方式？规则会保留，但这些快捷方式会回到原始启动方式。")) return;
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
    await saveConfig("已清理丢失的应用记录");
    toast("已清理丢失的应用记录");
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
        message: "未发现快捷方式",
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
    message: `扫描完成: 发现 ${count} 个快捷方式`,
    items
  });
  renderRules();
  toast(`发现 ${count} 个快捷方式`);
}

async function saveConfig(message = "已保存") {
  normalizeConfig();
  const result = await Api.saveConfig(appConfig);
  appConfig = result.config;
  normalizeConfig();
  applySettings();
  renderGroups();
  renderRules();
  loadAppStatuses().catch(() => {});
  if (appConfig.settings.auto_start && result.auto_start_synced === false) {
    toast(result.auto_start_error ? `启动项更新失败：${result.auto_start_error}` : "启动项更新失败");
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
    blocked: "已阻止",
    bypassed: "已允许本次启动",
    shortcut_restored: "快捷方式已还原",
    shortcut_restore_failed: "快捷方式还原失败",
    shortcut_replace_failed: "快捷方式替换失败",
    config_repaired: "配置已修复",
    protection_disabled_no_adapter: "未检测到适配器，已关闭防护",
    switched_to: "已切换网络",
    connect_requested: "已请求切换网络",
    native_dialog_opened: "已打开系统连接窗口",
    switch_failed: "切换失败"
  };
  return labels[key] || key;
}

async function loadWifiOptions() {
  const data = await Api.availableWifi();
  const names = new Set([...(data.known_profiles || []), ...(data.networks || []).map((item) => item.ssid)]);
  const options = [...names].filter(Boolean);
  $("wifiOptions").innerHTML = options.map((name) => `<option value="${escapeHtml(name)}"></option>`).join("");
  $("ruleWifiSelect").innerHTML = `<option value="">请选择 WiFi</option>` + options
    .map((name) => `<option value="${escapeHtml(name)}">${escapeHtml(name)}</option>`)
    .join("");
  renderRuleSummary();
}

async function loadStats() {
  const data = await Api.stats();
  const stats = data.stats || {};
  $("blockedToday").textContent = stats.blocked_today || 0;
  $("blockedWeek").textContent = stats.blocked_week || 0;
  $("topApp").textContent = stats.top_blocked_app || "暂无";
  $("logRows").innerHTML = (stats.records || []).slice(-80).reverse().map((row) => `
    <tr>
      <td>${escapeHtml(row.timestamp || "")}</td>
      <td>${escapeHtml(row.ssid || row.target_ssid || "")}</td>
      <td>${escapeHtml(row.app || row.shortcut || "")}</td>
      <td>${escapeHtml(actionLabel(row))}</td>
    </tr>
  `).join("");
}

async function init() {
  const data = await Api.getConfig();
  appConfig = data.config;
  normalizeConfig();
  $("configPath").textContent = data.config_path || "";
  applySettings();
  renderGroups();
  renderRules();
  loadAppStatuses().catch(() => {});
  loadWifiOptions().catch(() => {});

  if (location.hash === "#stats") setTab("stats");

  document.querySelectorAll(".nav-item").forEach((button) => {
    button.addEventListener("click", () => setTab(button.dataset.tab));
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
      toast(`已选择 ${shortcuts.length} 个快捷方式`);
    } catch (error) {
      toast(error.message);
    }
  });

  $("addRuleButton").addEventListener("click", resetRuleForm);
  $("restoreAllShortcutsButton").addEventListener("click", restoreAllShortcutsFromUi);
  $("ruleWifiSelect").addEventListener("change", renderRuleSummary);
  $("manualSsid").addEventListener("input", renderRuleSummary);
  $("ruleGroupSelect").addEventListener("change", renderRuleSummary);
  $("ruleForm").addEventListener("submit", saveRuleFromForm);

  $("protectionToggle").addEventListener("change", async (event) => {
    let message = event.target.checked ? "防护已开启" : "防护已关闭，快捷方式已尝试还原";
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
    await saveConfig("启动项已更新");
  });
  $("darkToggle").addEventListener("change", async (event) => {
    appConfig.settings.dark_mode = event.target.checked;
    await saveConfig("主题已更新");
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
    await saveConfig("密码已保存");
  });

  $("refreshStats").addEventListener("click", loadStats);
  startWifiPolling((wifi) => {
    $("currentSsid").textContent = wifi.adapter_available === false ? "未检测到 WiFi 适配器" : (wifi.ssid ? `已连接 ${wifi.ssid}` : "未连接");
  }, 5000);
}

init().catch((error) => toast(error.message));
