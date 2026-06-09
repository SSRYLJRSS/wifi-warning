function startWifiPolling(callback, interval = 5000) {
  let active = true;
  async function tick() {
    if (!active) return;
    try {
      const wifi = await Api.currentWifi();
      callback(wifi);
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
