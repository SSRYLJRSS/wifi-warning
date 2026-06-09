function networkRow(network) {
  const row = document.createElement("div");
  row.className = "network-row";
  row.innerHTML = `
    <div>
      <strong>${network.ssid}</strong>
      <div class="signal">${network.auth || "安全网络"} · 信号 ${network.signal_quality || 0}%</div>
    </div>
    <span class="pill">${network.connected ? "已连接" : network.secure ? "加密" : "开放"}</span>
    <button class="primary-button">连接</button>
  `;
  row.querySelector("button").addEventListener("click", async () => {
    try {
      await Api.switchWifi(network.ssid);
      toast(`已请求连接 ${network.ssid}`);
    } catch (error) {
      toast(error.message);
    }
  });
  return row;
}

async function loadNetworks() {
  const list = document.getElementById("networkList");
  list.innerHTML = `<div class="settings-card muted">正在扫描...</div>`;
  const data = await Api.availableWifi();
  list.innerHTML = "";
  const networks = data.networks || [];
  if (!networks.length) {
    list.innerHTML = `<div class="settings-card muted">未发现可用 WiFi。</div>`;
    return;
  }
  networks.forEach((network) => list.appendChild(networkRow(network)));
}

document.getElementById("refreshNetworks").addEventListener("click", () => {
  loadNetworks().catch((error) => toast(error.message));
});

loadNetworks().catch((error) => toast(error.message));
