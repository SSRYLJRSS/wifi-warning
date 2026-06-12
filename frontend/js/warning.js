const query = params();
let initialNetworkType = query.networkType || (query.ssid ? "wifi" : "");
let initialNetworkId = query.networkId || query.ssid || "";
let rule = null;
let appPath = query.app || "";
let appArgs = query.appArgs || "";

function text(id, value) {
  document.getElementById(id).textContent = value;
}

function networkLabel(type, name) {
  const prefix = type === "wired" ? t("wired") : t("wifi");
  return name ? `${prefix}: ${name}` : t("unknown");
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

async function configurePrimaryAction() {
  const button = document.getElementById("switchSafe");
  const ruleType = rule?.network_type || initialNetworkType || "wifi";
  if (ruleType === "wired") {
    const adapterId = rule?.network_id || initialNetworkId;
    const adapterName = rule?.network_name || adapterId;
    button.disabled = !adapterId;
    button.textContent = t("wiredDisconnect");
    button.onclick = async () => {
      if (!adapterId) return;
      try {
        await Api.toggleWired(adapterId, false);
        toast(t("wiredActionRequested", { action: t("disconnect"), name: adapterName }));
      } catch (error) {
        toast(error.message);
      }
    };
    return;
  }

  const safe = rule?.safe_wifi_ssid || "";
  button.textContent = safe ? t("switchSafe", { name: safe }) : t("noSafeNetwork");
  button.disabled = !safe;
  button.onclick = async () => {
    if (!safe) return;
    try {
      await Api.switchWifi(safe, rule?.safe_wifi_password || "");
      toast(t("requestSwitch"));
    } catch (error) {
      toast(error.message);
    }
  };
}

async function initWarning() {
  const configData = await Api.getConfig();
  const config = configData.config || {};
  I18N.setLanguage(currentLanguageFromConfig(config));

  text("appName", query.appName || (appPath.split(/[\\/]/).pop() || t("app")));
  text("ssid", networkLabel(initialNetworkType, initialNetworkId));

  loadAppIcon();
  rule = (config.rules || []).find((item) => item.id === query.ruleId);
  if (rule) {
    rule.network_type ||= rule.ssid ? "wifi" : initialNetworkType;
    rule.network_id ||= rule.ssid || initialNetworkId;
    rule.network_name ||= rule.network_id;
  }
  text("ruleDescription", rule?.description || t("warningRuleFallback"));
  await configurePrimaryAction();

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
      const hashedPassword = await sha256(document.getElementById("password").value);
      await Api.bypass({
        password: hashedPassword,
        app: appPath,
        app_args: appArgs,
        rule_id: query.ruleId
      });
      toast(t("allowedLaunch"));
      setTimeout(() => window.close(), 800);
    } catch (error) {
      toast(error.message);
    }
  });

  startNetworkPolling((network) => {
    if (!network.id) return;
    const name = network.name || network.id;
    document.getElementById("statusLine").textContent = t("detectedNetwork", { name: networkLabel(network.type, name) });
    if (initialNetworkId && (network.type || "") === initialNetworkType && network.id !== initialNetworkId) {
      window.close();
      document.body.innerHTML = "";
    }
  }, 2000);
}

initWarning().catch((error) => toast(error.message));
