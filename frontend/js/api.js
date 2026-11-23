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
      data = { ok: false, error: text || "Response parse failed" };
    }
    if (!response.ok || data.ok === false) {
      throw new Error(data.error || `Request failed ${response.status}`);
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

  currentNetwork() {
    return this.request("/api/network/current");
  },

  wiredAdapters() {
    return this.request("/api/network/wired");
  },

  toggleWired(id, enabled) {
    return this.request("/api/network/wired/toggle", {
      method: "POST",
      body: JSON.stringify({ id, enabled })
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

  /* IFEO interception (replaces the retired .lnk management) */
  Intercept: {
    status() {
      return Api.request("/api/intercept/status");
    },
    apply() {
      return Api.request("/api/intercept/apply", { method: "POST", body: "{}" });
    },
    restore() {
      return Api.request("/api/intercept/restore", { method: "POST", body: "{}" });
    }
  }
};

async function sha256(text) {
  const data = new TextEncoder().encode(text);
  const hash = await crypto.subtle.digest("SHA-256", data);
  return Array.from(new Uint8Array(hash)).map(b => b.toString(16).padStart(2, "0")).join("");
}

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
