const query = params();
let initialSsid = query.ssid || "";
let rule = null;
let appPath = query.app || "";
let appArgs = query.appArgs || "";

function text(id, value) {
  document.getElementById(id).textContent = value;
}

function loadAppIcon() {
  if (!appPath) return;
  const icon = document.getElementById("appIcon");
  const fallback = document.getElementById("fallbackIcon");
  if (!icon || !fallback) return;
  icon.addEventListener("load", () => {
    fallback.classList.add("hidden");
    icon.classList.remove("hidden");
  }, { once: true });
  icon.addEventListener("error", () => {
    icon.classList.add("hidden");
    fallback.classList.remove("hidden");
  }, { once: true });
  icon.src = Api.appIconUrl(appPath);
}

async function initWarning() {
  text("appName", query.appName || (appPath.split(/[\\/]/).pop() || "应用"));
  text("ssid", initialSsid || "未知");

  loadAppIcon();
  const configData = await Api.getConfig();
  const config = configData.config;
  rule = (config.rules || []).find((item) => item.id === query.ruleId);
  text("ruleDescription", rule?.description || "当前网络限制该应用启动");

  const safe = rule?.safe_wifi_ssid || "";
  document.getElementById("switchSafe").textContent = safe ? `切换到 ${safe}` : "未设置安全网络";
  document.getElementById("switchSafe").disabled = !safe;
  document.getElementById("switchSafe").addEventListener("click", async () => {
    if (!safe) return;
    try {
      await Api.switchWifi(safe, rule?.safe_wifi_password || "");
      toast("已请求切换网络");
    } catch (error) {
      toast(error.message);
    }
  });

  document.getElementById("pickWifi").addEventListener("click", () => {
    window.open("/wifi-picker", "wifi-picker", "width=760,height=680");
  });
  document.getElementById("showBypass").addEventListener("click", () => {
    document.getElementById("bypassForm").classList.toggle("hidden");
    document.getElementById("password").focus();
  });
  document.getElementById("closePage").addEventListener("click", () => window.close());

  document.getElementById("bypassForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    try {
      await Api.bypass({
        password: document.getElementById("password").value,
        app: appPath,
        app_args: appArgs,
        rule_id: query.ruleId
      });
      toast("已允许启动");
      setTimeout(() => window.close(), 800);
    } catch (error) {
      toast(error.message);
    }
  });

  startWifiPolling((wifi) => {
    if (!wifi.ssid) return;
    document.getElementById("statusLine").textContent = `当前检测到 ${wifi.ssid}`;
    if (initialSsid && wifi.ssid !== initialSsid) {
      window.close();
      document.body.innerHTML = "";
    }
  }, 2000);
}

initWarning().catch((error) => toast(error.message));
