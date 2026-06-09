const fs = require("fs");
const path = require("path");
const { chromium } = require("playwright");

function fail(message) {
  throw new Error(message);
}

async function waitForServer(baseUrl) {
  const deadline = Date.now() + 6000;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`${baseUrl}/api/config`);
      if (response.ok) return;
    } catch {
      // keep polling until the self-test server is ready
    }
    await new Promise((resolve) => setTimeout(resolve, 150));
  }
  fail(`server did not respond at ${baseUrl}`);
}

async function visibleText(page) {
  return page.locator("body").innerText({ timeout: 3000 });
}

async function assertNoPageErrors(page, label, errors) {
  if (errors.length) fail(`${label} browser errors: ${errors.join(" | ")}`);
  const failedRequests = await page.evaluate(() => {
    return (window.__failedResources || []).slice();
  });
  if (failedRequests.length) fail(`${label} failed requests: ${failedRequests.join(" | ")}`);
  const badStatus = await page.evaluate(() => {
    const entries = performance.getEntriesByType("resource")
      .filter((entry) => entry.responseStatus && entry.responseStatus >= 400)
      .map((entry) => `${entry.name} ${entry.responseStatus}`);
    return entries;
  });
  if (badStatus.length) fail(`${label} failed resources: ${badStatus.join(" | ")}`);
}

async function assertNoHorizontalOverflow(page, label) {
  const overflow = await page.evaluate(() => {
    const root = document.documentElement;
    return {
      scrollWidth: root.scrollWidth,
      clientWidth: root.clientWidth,
      bodyScrollWidth: document.body.scrollWidth,
      bodyClientWidth: document.body.clientWidth
    };
  });
  if (overflow.scrollWidth > overflow.clientWidth + 2 || overflow.bodyScrollWidth > overflow.bodyClientWidth + 2) {
    fail(`${label} has horizontal overflow: ${JSON.stringify(overflow)}`);
  }
}

async function assertMinimumTapTargets(page, label) {
  const tooSmall = await page.evaluate(() => {
    return Array.from(document.querySelectorAll("button, input, textarea, select"))
      .filter((element) => {
        const style = getComputedStyle(element);
        if (style.display === "none" || style.visibility === "hidden") return false;
        const rect = element.getBoundingClientRect();
        return rect.width > 0 && rect.height > 0 && (rect.height < 30 || rect.width < 30);
      })
      .map((element) => {
        const rect = element.getBoundingClientRect();
        return `${element.id || element.textContent.trim() || element.tagName}:${Math.round(rect.width)}x${Math.round(rect.height)}`;
      });
  });
  if (tooSmall.length) fail(`${label} has too-small controls: ${tooSmall.join(", ")}`);
}

async function screenshot(page, outDir, name) {
  await page.screenshot({ path: path.join(outDir, `${name}.png`), fullPage: true });
}

async function checkSettingsRuleCrud(page, baseUrl) {
  const shortcutPath = process.env.WW_BROWSER_SMOKE_SHORTCUT_PATH || "";
  await page.click('[data-tab="groups"]');
  await page.waitForSelector("#groupsPanel.active", { timeout: 3000 });
  await page.click("#newGroupButton");
  await page.fill("#groupName", "CRUD 黑名单");
  await Promise.all([
    page.waitForResponse((response) => response.url().includes("/api/shortcuts/browse") && response.request().method() === "POST", { timeout: 3000 }),
    page.click("#browseGroupShortcuts")
  ]);
  await page.waitForSelector('#groupShortcutList:has-text("CRUD App")', { timeout: 3000 });
  await Promise.all([
    page.waitForResponse((response) => response.url().includes("/api/config") && response.request().method() === "POST", { timeout: 3000 }),
    page.click("#groupForm button[type='submit']")
  ]);
  await page.waitForSelector('.software-group-card:has-text("CRUD 黑名单")', { timeout: 3000 });

  await page.click('[data-tab="rules"]');
  await page.waitForSelector("#rulesPanel.active", { timeout: 3000 });
  await page.selectOption("#ruleNetworkSelect", "wifi:Cafe-WiFi").catch(async () => {
    await page.fill("#manualNetwork", "Cafe-WiFi");
  });
  await page.selectOption("#ruleGroupSelect", { label: "CRUD 黑名单 (1)" });
  await Promise.all([
    page.waitForResponse((response) => response.url().includes("/api/config") && response.request().method() === "POST", { timeout: 3000 }),
    page.click("#saveRuleButton")
  ]);
  await page.waitForSelector('.rule-card:has-text("Cafe-WiFi") .shortcut-preview', { timeout: 8000 });
  await page.waitForFunction(async ({ url, shortcutPath }) => {
    const response = await fetch(`${url}/api/config`);
    const data = await response.json();
    return data.config?.rules?.some((rule) => rule.network_type === "wifi" && rule.network_id === "Cafe-WiFi" && rule.app_group_id && rule.blocked_apps?.some((app) => {
      return app.name === "CRUD App" && (!shortcutPath || app.shortcut_paths?.includes(shortcutPath));
    }));
  }, { url: baseUrl, shortcutPath }, { timeout: 3000 });

  let createdCard = page.locator(".rule-card", { hasText: "Cafe-WiFi" });
  await createdCard.locator('[data-action="edit"]').click();
  await page.fill("#manualNetwork", "Cafe-Updated");
  await Promise.all([
    page.waitForResponse((response) => response.url().includes("/api/config") && response.request().method() === "POST", { timeout: 3000 }),
    page.click("#saveRuleButton")
  ]);
  await page.waitForSelector('.rule-card:has-text("Cafe-Updated") .shortcut-preview', { timeout: 8000 });
  await page.waitForFunction(async (url) => {
    const response = await fetch(`${url}/api/config`);
    const data = await response.json();
    return data.config?.rules?.some((rule) => rule.network_type === "wifi" && rule.network_id === "Cafe-Updated" && rule.blocked_apps?.some((app) => app.name === "CRUD App"));
  }, baseUrl, { timeout: 3000 });

  createdCard = page.locator(".rule-card", { hasText: "Cafe-Updated" });
  page.once("dialog", async (dialog) => dialog.accept());
  await Promise.all([
    page.waitForResponse((response) => response.url().includes("/api/config") && response.request().method() === "POST", { timeout: 3000 }),
    createdCard.locator('[data-action="delete"]').click()
  ]);
  await page.waitForFunction(async (url) => {
    const response = await fetch(`${url}/api/config`);
    const data = await response.json();
    return !data.config?.rules?.some((rule) => rule.network_id === "Cafe-Updated");
  }, baseUrl, { timeout: 3000 });
}

async function checkSettingsMissingAppCleanup(page, baseUrl) {
  await page.click('[data-tab="rules"]');
  const runtimeCard = page.locator(".rule-card", { hasText: "Office-WiFi" });
  const missingRow = runtimeCard.locator(".app-status-row.missing", { hasText: "Missing App" });
  await missingRow.waitFor({ timeout: 5000 });
  await Promise.all([
    page.waitForResponse((response) => response.url().includes("/api/apps/cleanup") && response.request().method() === "POST", { timeout: 3000 }),
    missingRow.locator('[data-action="cleanup-app"]').click()
  ]);
  await page.waitForFunction(async (url) => {
    const response = await fetch(`${url}/api/config`);
    const data = await response.json();
    return !data.config?.rules?.some((rule) => rule.blocked_apps?.some((app) => app.name === "Missing App"));
  }, baseUrl, { timeout: 3000 });
  await page.waitForFunction(() => !document.body.innerText.includes("Missing App"), { timeout: 3000 });
}

async function checkSettingsPassword(page, baseUrl) {
  await page.click('[data-tab="password"]');
  await page.waitForSelector("#passwordPanel.active", { timeout: 3000 });
  await page.fill("#bypassPassword", "secret");
  await Promise.all([
    page.waitForResponse((response) => response.url().includes("/api/config") && response.request().method() === "POST", { timeout: 3000 }),
    page.click("#passwordForm button[type='submit']")
  ]);
  await page.waitForFunction(async (url) => {
    const response = await fetch(`${url}/api/config`);
    const data = await response.json();
    const settings = data.config?.settings || {};
    return settings.bypass_password === "2bb80d537b1da3e38bd30361aa855686bde0eacd7162fef6a25fe97bf527a25b"
      && settings.bypass_timeout_minutes === 0
      && settings.bypass_until_epoch === 0;
  }, baseUrl, { timeout: 3000 });
}

async function checkSettings(page, baseUrl, outDir) {
  const errors = [];
  const failedResponses = [];
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("console", (msg) => {
    if (msg.type() === "error" && !msg.text().includes("Failed to load resource")) errors.push(msg.text());
  });
  page.on("response", (response) => {
    if (response.status() >= 400) failedResponses.push(`${response.url()} ${response.status()}`);
  });
  await page.addInitScript(() => {
    window.__failedResources = [];
    window.__closeRequested = false;
    window.close = () => { window.__closeRequested = true; };
    window.addEventListener("error", (event) => {
      const target = event.target;
      if (target && target !== window && target.src) window.__failedResources.push(target.src);
      if (target && target !== window && target.href) window.__failedResources.push(target.href);
    }, true);
  });
  await page.goto(`${baseUrl}/settings`, { waitUntil: "networkidle" });
  await page.waitForSelector("#rulesList .rule-card", { timeout: 5000 });
  const text = await visibleText(page);
  for (const expected of ["网络规则", "软件组", "确定规则", "一键恢复快捷方式"]) {
    if (!text.includes(expected)) fail(`settings page missing ${expected}`);
  }
  await page.click('[data-lang="en-US"]');
  await page.waitForFunction(() => document.body.innerText.includes("Network rules") && document.body.innerText.includes("App groups"), { timeout: 3000 });
  await page.click('[data-lang="zh-CN"]');
  await page.waitForFunction(() => document.body.innerText.includes("网络规则") && document.body.innerText.includes("软件组"), { timeout: 3000 });
  await assertNoHorizontalOverflow(page, "settings screenshot");
  await screenshot(page, outDir, "settings-desktop");
  await page.click('[data-tab="groups"]');
  await page.waitForSelector("#groupsPanel.active", { timeout: 3000 });
  const groupsText = await visibleText(page);
  for (const expected of ["快捷方式黑名单", "Runtime App", "Runtime App.lnk"]) {
    if (!groupsText.includes(expected)) fail(`groups page missing ${expected}`);
  }
  await page.click('[data-tab="stats"]');
  await page.waitForSelector("#statsPanel.active", { timeout: 3000 });
  await page.click('[data-tab="about"]');
  await page.waitForSelector("#aboutPanel.active", { timeout: 3000 });
  const aboutText = await visibleText(page);
  for (const expected of ["开源许可", "GitHub"]) {
    if (!aboutText.includes(expected)) fail(`about page missing ${expected}`);
  }
  const autoStart = page.locator("#autoStartToggle");
  if (!(await autoStart.isChecked())) await autoStart.check();
  await page.waitForFunction(async (url) => {
    const response = await fetch(`${url}/api/config`);
    const data = await response.json();
    return data.config?.settings?.auto_start === true && data.auto_start_registered === true;
  }, baseUrl, { timeout: 3000 });
  await checkSettingsRuleCrud(page, baseUrl);
  page.once("dialog", async (dialog) => dialog.accept());
  await Promise.all([
    page.waitForResponse((response) => response.url().includes("/api/shortcuts/restore") && response.request().method() === "POST", { timeout: 3000 }),
    page.click("#restoreAllShortcutsButton")
  ]);
  await checkSettingsMissingAppCleanup(page, baseUrl);
  await checkSettingsPassword(page, baseUrl);
  await assertNoPageErrors(page, "settings", errors.concat(failedResponses));
  await assertNoHorizontalOverflow(page, "settings");
  await assertMinimumTapTargets(page, "settings");
}

async function checkWarning(page, baseUrl, outDir) {
  const errors = [];
  const failedResponses = [];
  const wifiSwitchResponses = [];
  const bypassResponses = [];
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("console", (msg) => {
    if (msg.type() === "error" && !msg.text().includes("Failed to load resource")) errors.push(msg.text());
  });
  page.on("response", (response) => {
    if (response.status() >= 400) failedResponses.push(`${response.url()} ${response.status()}`);
    if (response.url().includes("/api/wifi/switch")) wifiSwitchResponses.push(response);
    if (response.url().includes("/api/bypass")) bypassResponses.push(response);
  });
  await page.addInitScript(() => {
    window.__failedResources = [];
    window.addEventListener("error", (event) => {
      const target = event.target;
      if (target && target !== window && target.src) window.__failedResources.push(target.src);
      if (target && target !== window && target.href) window.__failedResources.push(target.href);
    }, true);
  });
  const appPath = process.env.WW_BROWSER_SMOKE_APP_PATH || "C:\\Runtime\\App.exe";
  const url = `${baseUrl}/warning?appName=Runtime%20App&app=${encodeURIComponent(appPath)}&ssid=Office-WiFi&ruleId=runtime_rule`;
  await page.goto(url, { waitUntil: "networkidle" });
  await page.waitForSelector("#switchSafe", { timeout: 5000 });
  const text = await visibleText(page);
  for (const expected of ["Runtime App", "当前网络", "Office-WiFi", "切换到 Home-WiFi", "允许本次启动"]) {
    if (!text.includes(expected)) fail(`warning page missing ${expected}`);
  }
  const disabled = await page.locator("#switchSafe").isDisabled();
  if (disabled) fail("safe WiFi switch button should be enabled");
  await assertNoHorizontalOverflow(page, "warning screenshot");
  await screenshot(page, outDir, "warning-desktop");
  await Promise.all([
    page.waitForResponse((response) => response.url().includes("/api/wifi/switch") && response.request().method() === "POST", { timeout: 3000 }),
    page.click("#switchSafe")
  ]);
  const switchPayload = await wifiSwitchResponses[wifiSwitchResponses.length - 1].json();
  if (!switchPayload.ok || switchPayload.status !== "connected_with_password" || switchPayload.password_supplied !== true) {
    fail(`safe WiFi switch action failed: ${JSON.stringify(switchPayload)}`);
  }
  const popupPromise = page.waitForEvent("popup", { timeout: 3000 });
  await page.click("#pickWifi");
  const popup = await popupPromise;
  await popup.waitForLoadState("domcontentloaded");
  if (!popup.url().includes("/wifi-picker")) fail(`WiFi picker button opened unexpected URL: ${popup.url()}`);
  await popup.close();
  await page.click("#showBypass");
  await page.fill("#password", "secret");
  await Promise.all([
    page.waitForResponse((response) => response.url().includes("/api/bypass") && response.request().method() === "POST", { timeout: 3000 }),
    page.click("#bypassForm button[type='submit']")
  ]);
  const bypassPayload = await bypassResponses[bypassResponses.length - 1].json();
  if (!bypassPayload.ok) fail(`bypass action failed: ${JSON.stringify(bypassPayload)}`);
  await page.waitForFunction(() => window.__closeRequested === true, { timeout: 2000 });
  await page.evaluate(() => { window.__closeRequested = false; });
  await page.click("#closePage");
  const closeClicked = await page.evaluate(() => window.__closeRequested === true);
  if (!closeClicked) fail("close button did not request window close");
  await assertNoPageErrors(page, "warning", errors.concat(failedResponses));
  await assertNoHorizontalOverflow(page, "warning");
  await assertMinimumTapTargets(page, "warning");
}

async function checkPicker(page, baseUrl, outDir) {
  const errors = [];
  const failedResponses = [];
  const wifiSwitchResponses = [];
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("console", (msg) => {
    if (msg.type() === "error" && !msg.text().includes("Failed to load resource")) errors.push(msg.text());
  });
  page.on("response", (response) => {
    if (response.status() >= 400) failedResponses.push(`${response.url()} ${response.status()}`);
    if (response.url().includes("/api/wifi/switch")) wifiSwitchResponses.push(response);
  });
  await page.addInitScript(() => {
    window.__failedResources = [];
    window.addEventListener("error", (event) => {
      const target = event.target;
      if (target && target !== window && target.src) window.__failedResources.push(target.src);
      if (target && target !== window && target.href) window.__failedResources.push(target.href);
    }, true);
  });
  await page.goto(`${baseUrl}/wifi-picker`, { waitUntil: "networkidle" });
  await page.waitForSelector(".network-row", { timeout: 5000 });
  const text = await visibleText(page);
  for (const expected of ["选择网络", "Office-WiFi", "Home-WiFi", "连接", "Ethernet 1"]) {
    if (!text.includes(expected)) fail(`wifi picker missing ${expected}`);
  }
  await assertNoHorizontalOverflow(page, "wifi picker screenshot");
  await screenshot(page, outDir, "wifi-picker-desktop");
  const homeWifiButton = page.locator(".network-row", { hasText: "Home-WiFi" }).locator("button");
  await Promise.all([
    page.waitForResponse((response) => response.url().includes("/api/wifi/switch") && response.request().method() === "POST", { timeout: 3000 }),
    homeWifiButton.click()
  ]);
  const switchPayload = await wifiSwitchResponses[wifiSwitchResponses.length - 1].json();
  if (!switchPayload.ok || switchPayload.target_ssid !== "Home-WiFi") {
    fail(`wifi picker connect action failed: ${JSON.stringify(switchPayload)}`);
  }
  await assertNoPageErrors(page, "wifi picker", errors.concat(failedResponses));
  await assertNoHorizontalOverflow(page, "wifi picker");
  await assertMinimumTapTargets(page, "wifi picker");
}

async function checkStatsAfterActions(page, baseUrl) {
  await page.goto(`${baseUrl}/settings#stats`, { waitUntil: "networkidle" });
  await page.waitForSelector("#statsPanel.active", { timeout: 3000 });
  await page.click("#refreshStats");
  await page.waitForFunction(() => {
    const text = document.body.innerText;
    return text.includes("已允许本次启动") && text.includes("已切换网络") && text.includes("Home-WiFi");
  }, { timeout: 5000 });
  const blockedWeek = Number(await page.locator("#blockedWeek").innerText());
  if (!Number.isFinite(blockedWeek) || blockedWeek < 0) fail(`invalid blocked week metric: ${blockedWeek}`);
}

async function checkWarningAutoClose(page, baseUrl) {
  const ssidFile = process.env.WW_BROWSER_SMOKE_SSID_FILE;
  if (!ssidFile) fail("missing SSID file for warning auto-close smoke");
  fs.writeFileSync(ssidFile, "Office-WiFi", "ascii");
  await page.addInitScript(() => {
    window.__closeRequested = false;
    window.close = () => { window.__closeRequested = true; };
  });
  const appPath = process.env.WW_BROWSER_SMOKE_APP_PATH || "C:\\Runtime\\App.exe";
  await page.goto(`${baseUrl}/warning?appName=Runtime%20App&app=${encodeURIComponent(appPath)}&ssid=Office-WiFi&ruleId=runtime_rule`, { waitUntil: "networkidle" });
  await page.waitForSelector("#switchSafe", { timeout: 5000 });
  fs.writeFileSync(ssidFile, "Home-WiFi", "ascii");
  await page.waitForFunction(() => window.__closeRequested === true && document.body.innerHTML === "", { timeout: 5000 });
}

async function main() {
  const baseUrl = process.env.WW_BROWSER_SMOKE_BASE_URL;
  const outDir = process.env.WW_BROWSER_SMOKE_OUT_DIR;
  const edgePath = process.env.WW_BROWSER_SMOKE_EDGE;
  if (!baseUrl || !outDir || !edgePath) fail("missing browser smoke environment");
  fs.mkdirSync(outDir, { recursive: true });

  await waitForServer(baseUrl);
  const browser = await chromium.launch({ headless: true, executablePath: edgePath });
  try {
    const context = await browser.newContext({
      viewport: { width: 1366, height: 900 },
      deviceScaleFactor: 1
    });
    const page = await context.newPage();
    await checkSettings(page, baseUrl, outDir);
    await checkWarning(page, baseUrl, outDir);
    await checkPicker(page, baseUrl, outDir);
    await checkStatsAfterActions(page, baseUrl);
    const autoClosePage = await context.newPage();
    await checkWarningAutoClose(autoClosePage, baseUrl);
    await autoClosePage.close();
    await context.close();
  } finally {
    await browser.close();
  }
}

main().catch((error) => {
  console.error(error.stack || error.message || error);
  process.exit(1);
});
