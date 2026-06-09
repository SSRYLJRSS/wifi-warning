function startNetworkPolling(callback, interval = 5000) {
  let active = true;
  async function tick() {
    if (!active) return;
    try {
      callback(await Api.currentNetwork());
    } catch {
      callback({ connected: false, type: "", id: "", name: "" });
    }
    if (active) setTimeout(tick, interval);
  }
  tick();
  return () => {
    active = false;
  };
}

function startWifiPolling(callback, interval = 5000) {
  let active = true;
  async function tick() {
    if (!active) return;
    try {
      callback(await Api.currentWifi());
    } catch {
      callback({ connected: false, ssid: "" });
    }
    if (active) setTimeout(tick, interval);
  }
  tick();
  return () => {
    active = false;
  };
}
