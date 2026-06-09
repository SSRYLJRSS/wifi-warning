let currentConfig = null;

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#039;"
  })[char]);
}

function wifiRow(network) {
  const row = document.createElement("div");
  row.className = "network-row";
  row.innerHTML = `
    <div>
      <strong>${escapeHtml(network.ssid)}</strong>
      <div class="signal">${escapeHtml(network.auth || t("secureNetwork"))} · ${escapeHtml(t("signal", { quality: network.signal_quality || 0 }))}</div>
    </div>
    <span class="pill">${escapeHtml(network.connected ? t("connected") : network.secure ? t("encrypted") : t("openNetwork"))}</span>
    <button class="primary-button">${escapeHtml(t("connect"))}</button>
  `;
  row.querySelector("button").addEventListener("click", async () => {
    try {
      await Api.switchWifi(network.ssid);
      toast(t("requestSwitch"));
    } catch (error) {
      toast(error.message);
    }
  });
  return row;
}

function wiredRow(adapter) {
  const row = document.createElement("div");
  row.className = "network-row";
  const connected = adapter.connected || adapter.status === "up";
  row.innerHTML = `
    <div>
      <strong>${escapeHtml(adapter.name || adapter.id)}</strong>
      <div class="signal">${escapeHtml(t("wired"))} · ${escapeHtml(connected ? t("statusUp") : t("statusDown"))}</div>
    </div>
    <span class="pill">${escapeHtml(adapter.enabled === false ? t("disabled") : t("enabled"))}</span>
    <button class="${connected ? "ghost-button danger-text" : "primary-button"}">${escapeHtml(connected ? t("disconnect") : t("restore"))}</button>
  `;
  row.querySelector("button").addEventListener("click", async () => {
    try {
      await Api.toggleWired(adapter.id, connected ? false : true);
      toast(t("wiredActionRequested", { action: connected ? t("disconnect") : t("restore"), name: adapter.name || adapter.id }));
      setTimeout(() => loadNetworks().catch((error) => toast(error.message)), 800);
    } catch (error) {
      toast(error.message);
    }
  });
  return row;
}

async function loadNetworks() {
  const list = document.getElementById("networkList");
  list.innerHTML = `<div class="settings-card muted">${escapeHtml(t("scanning"))}</div>`;
  const [wifiData, wiredData] = await Promise.all([
    Api.availableWifi().catch(() => ({ networks: [] })),
    Api.wiredAdapters().catch(() => ({ adapters: [] }))
  ]);
  list.innerHTML = "";
  const rows = [];
  for (const network of wifiData.networks || []) rows.push(wifiRow(network));
  for (const adapter of wiredData.adapters || []) rows.push(wiredRow(adapter));
  if (!rows.length) {
    list.innerHTML = `<div class="settings-card muted">${escapeHtml(t("noNetworks"))}</div>`;
    return;
  }
  rows.forEach((row) => list.appendChild(row));
}

async function initPicker() {
  const data = await Api.getConfig();
  currentConfig = data.config || {};
  I18N.setLanguage(currentLanguageFromConfig(currentConfig));
  document.getElementById("refreshNetworks").addEventListener("click", () => {
    loadNetworks().catch((error) => toast(error.message));
  });
  await loadNetworks();
}

initPicker().catch((error) => toast(error.message));
