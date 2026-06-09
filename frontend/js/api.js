const Api = {
  async request(path, options = {}) {
    const response = await fetch(path, {
      headers: { "Content-Type": "application/json", ...(options.headers || {}) },
      ...options
    });
    const text = await response.text();
    let data = {};
    try {
      data = text ? JSON.parse(text) : {};
    } catch {
      data = { ok: false, error: text || "响应解析失败" };
    }
    if (!response.ok || data.ok === false) {
      throw new Error(data.error || `请求失败 ${response.status}`);
    }
    return data;
  },

  getConfig() {
    return this.request("/api/config");
  },

  saveConfig(config) {
    return this.request("/api/config", {
      method: "POST",
      body: JSON.stringify({ config })
    });
  },

  currentWifi() {
    return this.request("/api/wifi/current");
  },

  availableWifi() {
    return this.request("/api/wifi/available");
  },

  switchWifi(ssid, password = "") {
    return this.request("/api/wifi/switch", {
      method: "POST",
      body: JSON.stringify(password ? { ssid, password } : { ssid })
    });
  },

  stats() {
    return this.request("/api/stats");
  },

  bypass(payload) {
    return this.request("/api/bypass", {
      method: "POST",
      body: JSON.stringify(payload)
    });
  },

  browseApps() {
    return this.request("/api/apps/browse", {
      method: "POST",
      body: "{}"
    });
  },

  browseShortcuts() {
    return this.request("/api/shortcuts/browse", {
      method: "POST",
      body: "{}"
    });
  },

  readShortcut(shortcutPath) {
    return this.request("/api/shortcuts/read", {
      method: "POST",
      body: JSON.stringify({ shortcut_path: shortcutPath })
    });
  },

  appIconUrl(appPath) {
    return `/api/apps/icon?path=${encodeURIComponent(appPath)}`;
  },

  appStatus() {
    return this.request("/api/apps/status");
  },

  cleanupApp(ruleId, appPath) {
    return this.request("/api/apps/cleanup", {
      method: "POST",
      body: JSON.stringify({ rule_id: ruleId, app_path: appPath })
    });
  },

  scanShortcuts(appPath) {
    return this.request("/api/shortcuts/scan", {
      method: "POST",
      body: JSON.stringify({ app_path: appPath })
    });
  },

  replaceShortcuts(appPath, ruleId) {
    return this.request("/api/shortcuts/replace", {
      method: "POST",
      body: JSON.stringify({ app_path: appPath, rule_id: ruleId })
    });
  },

  restoreShortcuts(ruleId = "") {
    return this.request("/api/shortcuts/restore", {
      method: "POST",
      body: JSON.stringify(ruleId ? { rule_id: ruleId } : {})
    });
  }
};

function toast(message) {
  const el = document.getElementById("toast");
  if (!el) return;
  el.textContent = message;
  el.classList.remove("hidden");
  clearTimeout(window.__toastTimer);
  window.__toastTimer = setTimeout(() => el.classList.add("hidden"), 2600);
}

function params() {
  return Object.fromEntries(new URLSearchParams(location.search).entries());
}
