const pages = Array.from(document.querySelectorAll(".page"));
const navButtons = Array.from(document.querySelectorAll("[data-page-target]"));
const zoneButtons = Array.from(document.querySelectorAll("[data-zone]"));
const statusBar = document.querySelector(".app-status");
const statusBack = document.querySelector("#statusBack");
const statusPanel = document.querySelector("#statusPanel");
const statusClock = document.querySelector("#statusClock");
const standbyTime = document.querySelector(".standby-time");
const dock = document.querySelector(".dock");
const networkState = document.querySelector("#networkState");
const demoRun = document.querySelector("#demoRun");
const brightnessRange = document.querySelector("#brightnessRange");
const brightnessValue = document.querySelector("#brightnessValue");
const statusWifiSelect = document.querySelector("#statusWifiSelect");
const settingsWifiSelect = document.querySelector("#settingsWifiSelect");
const wifiList = document.querySelector("#wifiList");
const wifiDetailText = document.querySelector("#wifiDetailText");
const wifiModal = document.querySelector("#wifiModal");
const wifiDialogTitle = document.querySelector("#wifiDialogTitle");
const wifiPasswordInput = document.querySelector("#wifiPasswordInput");
const wifiDialogStatus = document.querySelector("#wifiDialogStatus");
const wifiCancel = document.querySelector("#wifiCancel");
const wifiConnect = document.querySelector("#wifiConnect");
const mascot = document.querySelector(".mascot");
const mascotFace = document.querySelector(".mascot-face");
const standbyMood = document.querySelector("#standbyMood");
const gridBoard = document.querySelector("#gridBoard");
const zoneTitle = document.querySelector("#zoneTitle");
const expiringFoodList = document.querySelector("#expiringFoodList");
const expiringCount = document.querySelector("#expiringCount");
const voiceButtons = Array.from(document.querySelectorAll("[data-voice-target]"));
const voiceStatus = document.querySelector("#voiceStatus");
const cameraVoiceStatus = document.querySelector("#cameraVoiceStatus");
const foodNameInput = document.querySelector("#foodNameInput");
const foodAmountInput = document.querySelector("#foodAmountInput");
const foodExpireInput = document.querySelector("#foodExpireInput");
const foodPlaceInput = document.querySelector("#foodPlaceInput");
const foodPlaceEditButton = document.querySelector("#foodPlaceEditButton");
const foodNoteInput = document.querySelector("#foodNoteInput");
const foodStoredInput = document.querySelector("#foodStoredInput");
const screen = document.querySelector("#screen");
const manualCaptureOpen = document.querySelector("#manualCaptureOpen");
const manualCaptureClose = document.querySelector("#manualCaptureClose");
const captureKeyboard = document.querySelector("#captureKeyboard");
const captureKeyboardKeys = document.querySelector("#captureKeyboardKeys");
const manualInputLine = document.querySelector("#manualInputLine");
const manualCapturePreview = document.querySelector("#manualCapturePreview");
const shutterButton = document.querySelector("#shutterButton");
const cameraResult = document.querySelector("#cameraResult");
const cameraFoodName = document.querySelector("#cameraFoodName");
const cameraFoodMeta = document.querySelector("#cameraFoodMeta");
const cameraConfirmButton = document.querySelector("#cameraConfirmButton");
const cameraPreview = document.querySelector("#cameraPreview");
const manualEditOpen = document.querySelector("#manualEditOpen");
const manualEditPreview = document.querySelector("#manualEditPreview");
const manualKeyboardTitle = document.querySelector("#manualKeyboardTitle");
const keyboardModeLabel = document.querySelector("#keyboardModeLabel");
const saveFoodButton = document.querySelector(".save-button");
const spaceEditToggle = document.querySelector("#spaceEditToggle");
const addFridgeZone = document.querySelector("#addFridgeZone");
const spaceEditor = document.querySelector("#spaceEditor");
const spaceNameInput = document.querySelector("#spaceNameInput");
const spaceWidthRange = document.querySelector("#spaceWidthRange");
const spaceHeightRange = document.querySelector("#spaceHeightRange");
const spaceNoteInput = document.querySelector("#spaceNoteInput");
const spaceDeleteButton = document.querySelector("#spaceDeleteButton");

let demoTimer = null;
let mascotTimer = null;
let themeTimer = null;
let inactivityTimer = null;
let powerTimer = null;
let currentPageName = "standby";
let currentZone = "left";
let currentSlot = "B2";
let currentFoodKey = "left:B2";
let manualCaptureText = "";
let manualInputTarget = "camera";
let keyboardLanguage = "zh";
let keyboardCase = "upper";
let keyboardPanel = "letters";
let pinyinBuffer = "";
let cameraRegistrationNote = "";
let cameraCapturedFood = null;
let cameraCaptureIndex = 0;
let cameraStream = null;
let pendingWifiNetwork = "";
let isSpaceEditing = false;
let isChoosingFoodPlace = false;
let editingFoodSource = { key: "left:B2", zone: "left", slot: "B2" };
let customZoneCount = 0;
let activeCustomZoneButton = null;

const wifiNetworks = {
  "Home_2.4G": { signal: "-58 dBm", secured: true },
  "Kitchen_5G": { signal: "-63 dBm", secured: true },
  "Fridge_IoT": { signal: "-51 dBm", secured: false },
  "Campus_IoT": { signal: "-71 dBm", secured: true },
};

const spacePalettes = {
  warm: ["#fff8e7", "#f3f8ed", "#f8f2e7", "#eef6f3", "#fff1e6"],
};

const DEFAULT_INACTIVITY_TIMEOUT_MS = 120000;

const cameraFoodSamples = [
  {
    name: "番茄",
    amount: "2 个",
    place: "建议放入左侧冷藏 B2",
    note: "外皮完整，适合 3 天内食用",
  },
  {
    name: "鸡蛋",
    amount: "6 个",
    place: "建议放入门架 A1",
    note: "已读取生产日期，建议一周内优先使用",
  },
  {
    name: "菠菜",
    amount: "1 把",
    place: "建议放入右侧冷藏 B1",
    note: "叶片新鲜，建议用保鲜袋包好",
  },
];

const pinyinCandidates = {
  fanqie: ["番茄"],
  xihongshi: ["西红柿"],
  jidan: ["鸡蛋"],
  niunai: ["牛奶"],
  qingcai: ["青菜"],
  bocai: ["菠菜"],
  shengcai: ["生菜"],
  huanggua: ["黄瓜"],
  huluobo: ["胡萝卜"],
  tudou: ["土豆"],
  pingguo: ["苹果"],
  xiangjiao: ["香蕉"],
  caomei: ["草莓"],
  putao: ["葡萄"],
  rou: ["肉"],
  zhurou: ["猪肉"],
  niurou: ["牛肉"],
  jirou: ["鸡肉"],
  yu: ["鱼"],
  xia: ["虾"],
  fan: ["饭"],
  miantiao: ["面条"],
  lengcang: ["冷藏"],
  lengdong: ["冷冻"],
  menjia: ["门架"],
  zuoce: ["左侧"],
  youce: ["右侧"],
  shangceng: ["上层"],
  xiaceng: ["下层"],
  jintian: ["今天"],
  mingtian: ["明天"],
  youxian: ["优先"],
  beizhu: ["备注"],
  shuliang: ["数量"],
  baozhi: ["保质"],
  guoqi: ["过期"],
  xinxian: ["新鲜"],
  jinshi: ["尽快食用"],
  youxianchishi: ["优先吃"],
  liangge: ["两个"],
  sange: ["三个"],
  yige: ["一个"],
};

const standbyFaces = [
  ["^_^", "今天也在认真保鲜"],
  ["-_-", "安静守着小番茄"],
  ["o_o", "库存一切正常"],
  ["=w=", "低功耗小憩中"],
  ["^o^", "牛奶还有 1 天"],
  ["u_u", "厨房很安静"],
  [">_<", "番茄想早点登场"],
];

const pageMeta = {
  standby: { hint: "待机", net: "休眠" },
  home: { hint: "首页", net: "Wi-Fi" },
  zone: { hint: "位置", net: "Wi-Fi" },
  editFood: { hint: "编辑", net: "Wi-Fi" },
  door: { hint: "提醒", net: "本地" },
  camera: { hint: "登记", net: "相机" },
  cameraResult: { hint: "确认", net: "相机" },
  recipe: { hint: "AI", net: "MQTT" },
  shopping: { hint: "清单", net: "云同步" },
  settings: { hint: "设置", net: "Wi-Fi" },
  wifi: { hint: "网络", net: "Wi-Fi" },
  more: { hint: "更多", net: "Wi-Fi" },
  offline: { hint: "离线", net: "重连中" },
};

const pageReturnTargets = {
  zone: "home",
  editFood: "zone",
  door: "home",
  camera: "home",
  cameraResult: "camera",
  recipe: "home",
  shopping: "home",
  settings: "home",
  wifi: "settings",
  more: "home",
  offline: "home",
};

const zoneNames = {
  freezer: "上层冷冻",
  left: "左侧冷藏",
  right: "右侧冷藏",
  door: "门架",
};

const zoneFoods = {
  freezer: [
    ["A1", "速冻水饺", "12 个"],
    ["A2", "鸡胸肉", "1 份"],
    ["A3", "空位", "可放冷冻"],
    ["B1", "玉米粒", "半袋"],
    ["B2", "虾仁", "200 g"],
    ["B3", "空位", "待补货"],
    ["C1", "冰袋", "备用"],
    ["C2", "牛肉卷", "1 盒"],
    ["C3", "空位", "可放肉类"],
  ],
  left: [
    ["A1", "鸡蛋", "3 个"],
    ["A2", "酸奶", "2 杯"],
    ["A3", "空位", "可放水果"],
    ["B1", "菠菜", "1 把"],
    ["B2", "番茄", "2 个"],
    ["B3", "黄瓜", "1 根"],
    ["C1", "豆腐", "1 盒"],
    ["C2", "空位", "待补货"],
    ["C3", "包装食品", "待确认"],
  ],
  right: [
    ["A1", "生菜", "1 颗"],
    ["A2", "胡萝卜", "2 根"],
    ["A3", "蘑菇", "1 盒"],
    ["B1", "菠菜", "1 把"],
    ["B2", "蓝莓", "1 盒"],
    ["B3", "空位", "可放蔬菜"],
    ["C1", "剩菜", "今晚吃"],
    ["C2", "苹果", "3 个"],
    ["C3", "空位", "待补货"],
  ],
  door: [
    ["A1", "番茄酱", "半瓶"],
    ["A2", "沙拉酱", "1 瓶"],
    ["A3", "空位", "可放调味"],
    ["B1", "果汁", "1 瓶"],
    ["B2", "黄油", "1 块"],
    ["B3", "空位", "待补货"],
    ["C1", "牛奶", "1 盒"],
    ["C2", "苏打水", "2 罐"],
    ["C3", "空位", "可放饮品"],
  ],
};

function getFoodKey(zone, slot) {
  return `${zone}:${slot}`;
}

function getSlotDisplayName(slot) {
  const vertical = { A: "内", B: "中", C: "外" }[slot?.[0]] || "中";
  const horizontal = { 1: "左", 2: "中", 3: "右" }[slot?.[1]] || "中";
  return `${vertical} · ${horizontal}`;
}

function getDisplayPlace(zone, slot) {
  return `${zoneNames[zone] || "冰箱区域"} ${getSlotDisplayName(slot)}`;
}

function normalizeDisplayPlace(place = "", zone = currentZone, slot = currentSlot) {
  return /[A-C][1-3]/i.test(place) ? getDisplayPlace(zone, slot) : place;
}

function replacePlaceInText(text = "", fromPlace = "", toPlace = "") {
  if (!text || !toPlace) return text;
  let nextText = text;
  if (fromPlace) {
    nextText = nextText.split(fromPlace).join(toPlace);
  }
  const legacyPlacePattern = /(?:上层冷冻|左侧冷藏|右侧冷藏|门架|自定义区 \d+)\s+[A-C][1-3]/gu;
  const displayPlacePattern = /(?:上层冷冻|左侧冷藏|右侧冷藏|门架|自定义区 \d+)\s+(?:内|中|外)\s·\s(?:左|中|右)/gu;
  return nextText.replace(legacyPlacePattern, toPlace).replace(displayPlacePattern, toPlace);
}

function padTime(value) {
  return String(value).padStart(2, "0");
}

function formatClock(date = new Date()) {
  return `${padTime(date.getHours())}:${padTime(date.getMinutes())}`;
}

function formatDateTime(value) {
  if (!value) return "暂无放入时间";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return "暂无放入时间";
  return `${date.getFullYear()}-${padTime(date.getMonth() + 1)}-${padTime(date.getDate())} ${formatClock(date)}`;
}

function formatShortDateTime(value) {
  if (!value) return "未放入";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return "未放入";
  return `${padTime(date.getMonth() + 1)}-${padTime(date.getDate())} ${formatClock(date)}`;
}

function getSampleStoredAt(zone, slot, empty) {
  if (empty) return "";
  const seed = Array.from(`${zone}${slot}`).reduce((total, char) => total + char.charCodeAt(0), 0);
  const now = new Date();
  const date = new Date();
  date.setDate(date.getDate() - (seed % 5));
  date.setHours(8 + (seed % 11), (seed * 7) % 60, 0, 0);
  if (date > now) {
    date.setDate(date.getDate() - 1);
  }
  return date.toISOString();
}

function buildFoodRecord(zone, slot, food, amount) {
  const empty = food === "空位";
  return {
    name: empty ? "" : food,
    amount,
    expire: amount.includes("待") || amount.includes("可放") ? "待设置" : "3 天后",
    place: getDisplayPlace(zone, slot),
    storedAt: getSampleStoredAt(zone, slot, empty),
    note: empty ? "可登记新食材。" : `来自${zoneNames[zone]} ${slot}，${amount}。`,
  };
}

const foodRecords = Object.fromEntries(
  Object.entries(zoneFoods).flatMap(([zone, foods]) => (
    foods.map(([slot, food, amount]) => [getFoodKey(zone, slot), buildFoodRecord(zone, slot, food, amount)])
  )),
);

const expiringFoods = [
  { zone: "left", slot: "A1", marker: "egg", status: "2 天后到期" },
  { zone: "door", slot: "C1", marker: "milk", status: "明天到期" },
  { zone: "right", slot: "B1", marker: "leaf", status: "适合煮汤" },
];

function getCurrentHour() {
  const params = new URLSearchParams(window.location.search);
  const hour = Number(params.get("hour"));
  if (Number.isInteger(hour) && hour >= 0 && hour <= 23) {
    return hour;
  }
  return new Date().getHours();
}

function getAutoTheme() {
  return "warm";
}

function isLowUseTime() {
  const hour = getCurrentHour();
  return (hour >= 0 && hour < 5) || (hour >= 14 && hour < 16);
}

function getInactivityTimeout() {
  const params = new URLSearchParams(window.location.search);
  const idle = Number(params.get("idle"));
  return Number.isInteger(idle) && idle >= 1000 ? idle : DEFAULT_INACTIVITY_TIMEOUT_MS;
}

function updateClockDisplay() {
  const now = new Date();
  const clockText = formatClock(now);
  statusClock.textContent = clockText;
  standbyTime.textContent = clockText;
}

function updateStandbyPowerMode() {
  const shouldScreenOff = currentPageName === "standby" && isLowUseTime();
  screen.classList.toggle("is-screen-off", shouldScreenOff);
  if (shouldScreenOff) {
    standbyMood.textContent = "低功耗息屏中";
  }
}

function setPage(name) {
  if (currentPageName !== name && screen.classList.contains("capture-keyboard-open")) {
    closeCaptureKeyboard();
  }
  currentPageName = name;
  pages.forEach((page) => {
    page.classList.toggle("is-active", page.dataset.page === name);
  });
  screen.classList.toggle("is-choosing-place", isChoosingFoodPlace);
  updatePlaceSelectionHints();

  navButtons.forEach((button) => {
    button.classList.toggle("is-selected", button.dataset.pageTarget === name);
  });

  const isStandby = name === "standby";
  statusBar.style.display = isStandby ? "none" : "flex";
  dock.style.display = isStandby ? "none" : "grid";
  statusPanel.classList.remove("is-open");
  networkState.textContent = pageMeta[name]?.net || "Wi-Fi";
  const canReturn = Boolean(pageReturnTargets[name]) || isChoosingFoodPlace;
  statusBack.classList.toggle("is-back", canReturn);
  statusBack.setAttribute("aria-label", canReturn ? "返回上一级" : "小精灵");
  if (name === "camera") {
    startCameraPreview();
  } else {
    stopCameraPreview();
  }
  updateStandbyPowerMode();
}

function updatePlaceSelectionHints() {
  document.querySelectorAll(".place-mode-hint").forEach((node) => node.remove());
  if (!isChoosingFoodPlace || !["home", "zone"].includes(currentPageName)) return;
  const page = document.querySelector(`.page[data-page="${currentPageName}"]`);
  const anchor = page?.querySelector(currentPageName === "home" ? ".home-head" : ".zone-top");
  if (!anchor) return;
  const hint = document.createElement("div");
  hint.className = "place-mode-hint";
  hint.textContent = currentPageName === "home" ? "位置编辑模式：请选择冰箱区域" : "位置编辑模式：请选择新的九宫格位置";
  anchor.insertAdjacentElement("afterend", hint);
}

async function startCameraPreview() {
  if (!navigator.mediaDevices?.getUserMedia || cameraStream) {
    return;
  }
  try {
    cameraStream = await navigator.mediaDevices.getUserMedia({
      video: { facingMode: "environment" },
      audio: false,
    });
    cameraPreview.srcObject = cameraStream;
    cameraPreview.classList.add("is-live");
  } catch {
    cameraPreview.classList.remove("is-live");
  }
}

function stopCameraPreview() {
  if (!cameraStream) return;
  cameraStream.getTracks().forEach((track) => track.stop());
  cameraStream = null;
  cameraPreview.srcObject = null;
  cameraPreview.classList.remove("is-live");
}

function setTheme(themeMode) {
  document.body.dataset.theme = "warm";
  document.body.dataset.themeMode = "warm";
  localStorage.setItem("fridgeTheme", "warm");
}

function refreshAutoTheme() {
  setTheme("warm");
  updateStandbyPowerMode();
}

function setBrightness(value) {
  const brightness = Math.min(120, Math.max(45, Number(value) || 100));
  screen.style.setProperty("--screen-brightness", String(brightness / 100));
  localStorage.setItem("fridgeBrightness", String(brightness));
  if (brightnessRange) {
    brightnessRange.value = String(brightness);
  }
  if (brightnessValue) {
    brightnessValue.textContent = `${brightness}%`;
  }
}

function syncWifiSelection(network) {
  if (statusWifiSelect) {
    statusWifiSelect.value = network;
  }
  if (settingsWifiSelect) {
    settingsWifiSelect.value = network;
  }
  if (wifiDetailText) {
    const wifi = wifiNetworks[network] || { signal: "-62 dBm", secured: true };
    wifiDetailText.textContent = `${network} · ${wifi.signal} · ${wifi.secured ? "已加密" : "开放网络"}`;
  }
  renderWifiList();
}

function applyWifiConnection(network, message = "") {
  localStorage.setItem("fridgeWifi", network);
  syncWifiSelection(network);
  networkState.textContent = "Wi-Fi";
  if (wifiDialogStatus && message) {
    wifiDialogStatus.textContent = message;
  }
}

function openWifiDialog(network) {
  pendingWifiNetwork = network;
  syncWifiSelection(network);
  wifiDialogTitle.textContent = network;
  wifiPasswordInput.value = "";
  wifiDialogStatus.textContent = "请输入密码以加入该 Wi-Fi 网络。";
  wifiModal.classList.add("is-open");
  wifiModal.setAttribute("aria-hidden", "false");
  window.setTimeout(() => {
    wifiPasswordInput.focus();
    openCaptureKeyboard("wifi");
  }, 0);
}

function closeWifiDialog() {
  const connectedNetwork = localStorage.getItem("fridgeWifi") || "Home_2.4G";
  pendingWifiNetwork = "";
  if (manualInputTarget === "wifi") {
    closeCaptureKeyboard();
  }
  syncWifiSelection(connectedNetwork);
  wifiModal.classList.remove("is-open");
  wifiModal.setAttribute("aria-hidden", "true");
}

function handleWifiSelection(network) {
  const wifi = wifiNetworks[network];
  if (!wifi) return;
  if (wifi.secured) {
    openWifiDialog(network);
    return;
  }
  pendingWifiNetwork = "";
  applyWifiConnection(network);
  if (wifiDialogStatus) {
    wifiDialogStatus.textContent = `${network} 为开放网络，已直接连接。`;
  }
  renderWifiList();
}

function connectWifi() {
  const password = wifiPasswordInput.value.trim();
  if (!password) {
    wifiDialogStatus.textContent = "请输入 Wi-Fi 密码后再连接。";
    return;
  }
  applyWifiConnection(pendingWifiNetwork, `已连接 ${pendingWifiNetwork}`);
  window.setTimeout(() => {
    pendingWifiNetwork = "";
    if (manualInputTarget === "wifi") {
      closeCaptureKeyboard();
    }
    wifiModal.classList.remove("is-open");
    wifiModal.setAttribute("aria-hidden", "true");
    renderWifiList();
  }, 450);
}

function renderWifiList() {
  if (!wifiList) return;
  const connectedNetwork = localStorage.getItem("fridgeWifi") || "Home_2.4G";
  wifiList.innerHTML = "";

  Object.entries(wifiNetworks).forEach(([name, wifi]) => {
    const article = document.createElement("article");
    const content = document.createElement("div");
    const title = document.createElement("strong");
    const meta = document.createElement("span");
    const button = document.createElement("button");

    title.textContent = name;
    meta.textContent = `${wifi.signal} · ${wifi.secured ? "加密网络" : "开放网络"}${name === connectedNetwork ? " · 当前已连接" : ""}`;
    button.type = "button";
    button.textContent = name === connectedNetwork ? "重新连接" : "连接";
    button.addEventListener("click", () => handleWifiSelection(name));

    content.append(title, meta);
    article.append(content, button);
    wifiList.appendChild(article);
  });
}

function appendNote(text) {
  const prefix = foodNoteInput.value.trim() ? "\n" : "";
  foodNoteInput.value = `${foodNoteInput.value.trim()}${prefix}${text}`;
}

function appendCurrentFoodNote(text) {
  appendNote(text);
  if (foodRecords[currentFoodKey]) {
    foodRecords[currentFoodKey].note = foodNoteInput.value;
  }
}

function appendCameraRegistrationNote(text) {
  const prefix = cameraRegistrationNote ? "\n" : "";
  cameraRegistrationNote = `${cameraRegistrationNote}${prefix}${text}`;
  manualCapturePreview.textContent = cameraRegistrationNote;
  if (cameraCapturedFood) {
    cameraCapturedFood.note = cameraRegistrationNote;
    updateCameraResult();
  }
}

function updateCameraResult() {
  if (!cameraCapturedFood) return;
  cameraResult.classList.add("is-ready");
  cameraFoodName.textContent = cameraCapturedFood.name;
  cameraFoodMeta.textContent = `${cameraCapturedFood.amount} · ${cameraCapturedFood.place} · ${cameraCapturedFood.placementReason || "按当前库存推荐"} · ${cameraCapturedFood.note}`;
  shutterButton.disabled = true;
  cameraConfirmButton.disabled = false;
}

function captureFoodFromCamera() {
  if (cameraCapturedFood) return;
  cameraCapturedFood = { ...cameraFoodSamples[cameraCaptureIndex % cameraFoodSamples.length] };
  cameraCaptureIndex += 1;
  const recommended = recommendFoodPlacement(cameraCapturedFood.name);
  cameraCapturedFood.place = recommended.place;
  cameraCapturedFood.placementReason = recommended.reason;
  cameraRegistrationNote = cameraCapturedFood.note;
  updateCameraResult();
  manualCapturePreview.textContent = cameraCapturedFood.note;
  cameraVoiceStatus.textContent = "已读取食材信息，可继续语音或手动修改。";
  setPage("cameraResult");
}

function parseCameraFoodPlace(placeText = "") {
  const zoneEntry = Object.entries(zoneNames).find(([, label]) => placeText.includes(label));
  const slotMatch = placeText.match(/[A-C][1-3]/i);
  return {
    zone: zoneEntry?.[0] || currentZone || "left",
    slot: slotMatch ? slotMatch[0].toUpperCase() : currentSlot || "A1",
  };
}

function getZoneItemCount(zone) {
  return (zoneFoods[zone] || []).filter(([, food]) => food && food !== "空位").length;
}

function getSlotPlacement(zone, slot, reason) {
  return {
    zone,
    slot,
    reason,
    place: `建议放入${getDisplayPlace(zone, slot)}`,
  };
}

function recommendFoodPlacement(foodName) {
  const sameKind = Object.entries(foodRecords).find(([, record]) => record.name && record.name === foodName);
  if (sameKind) {
    const [zone, slot] = sameKind[0].split(":");
    return getSlotPlacement(zone, slot, "同类食材优先合并");
  }

  const candidates = Object.entries(zoneFoods).flatMap(([zone, foods]) => (
    foods.map(([slot, food]) => ({ zone, slot, food, zoneCount: getZoneItemCount(zone) }))
  ));
  const emptySlot = candidates
    .filter(({ food }) => !food || food === "空位")
    .sort((a, b) => a.zoneCount - b.zoneCount)[0];
  if (emptySlot) {
    return getSlotPlacement(emptySlot.zone, emptySlot.slot, "优先选择空余位置");
  }

  const fallback = candidates.sort((a, b) => a.zoneCount - b.zoneCount)[0] || { zone: "left", slot: "A1" };
  return getSlotPlacement(fallback.zone, fallback.slot, "无空位时选择物品较少区域");
}

function parseAmountParts(amount = "") {
  const match = amount.trim().match(/^(\d+(?:\.\d+)?)\s*(.*)$/u);
  if (!match) return null;
  return {
    value: Number(match[1]),
    unit: match[2].trim(),
  };
}

function formatMergedAmount(value, unit) {
  return `${Number.isInteger(value) ? value : Number(value.toFixed(1))}${unit ? ` ${unit}` : ""}`;
}

function mergeFoodAmounts(currentAmount = "", addedAmount = "") {
  const current = parseAmountParts(currentAmount);
  const added = parseAmountParts(addedAmount);
  if (current && added && current.unit === added.unit) {
    return formatMergedAmount(current.value + added.value, current.unit);
  }
  return `${currentAmount || "待确认"} + ${addedAmount || "待确认"}`;
}

function appendRecordNote(record, text) {
  const prefix = record.note?.trim() ? "\n" : "";
  record.note = `${record.note || ""}${prefix}${text}`;
}

function writeCameraFoodToInventory(food) {
  const { zone, slot } = parseCameraFoodPlace(food.place);
  if (!zoneFoods[zone]) return null;

  const itemIndex = zoneFoods[zone].findIndex(([itemSlot]) => itemSlot === slot);
  const recordKey = getFoodKey(zone, slot);
  const existingRecord = foodRecords[recordKey];
  const storedAt = new Date().toISOString();
  const isSameKind = existingRecord?.name && existingRecord.name === food.name;
  const record = isSameKind ? existingRecord : {
    name: food.name,
    amount: food.amount || "待确认",
    expire: "3 天后",
    place: getDisplayPlace(zone, slot),
    storedAt,
    note: food.note || "拍照登记后放入。",
  };

  if (isSameKind) {
    const previousAmount = record.amount || "待确认";
    record.amount = mergeFoodAmounts(previousAmount, food.amount);
    record.place = getDisplayPlace(zone, slot);
    appendRecordNote(record, `拍照补入 ${food.amount || "待确认"}，数量由 ${previousAmount} 更新为 ${record.amount}。${food.note || ""}`);
  }

  foodRecords[recordKey] = record;
  if (itemIndex >= 0) {
    zoneFoods[zone][itemIndex] = [slot, record.name || "空位", record.amount];
  } else {
    zoneFoods[zone].push([slot, record.name || "空位", record.amount]);
  }

  if (currentZone === zone) {
    currentSlot = slot;
    renderZone(zone);
  }
  updateFridgeOverviewCounts();
  renderExpiringFoods();
  return record;
}

function updateFridgeOverviewCounts() {
  Object.keys(zoneFoods).forEach((zone) => {
    const button = document.querySelector(`.fridge-map [data-zone="${zone}"]`);
    const countTarget = button?.querySelector("strong");
    if (!countTarget) return;
    const count = zoneFoods[zone].filter(([, food]) => food && food !== "空位").length;
    countTarget.textContent = count > 0 ? `${count} 件` : "可放食材";
  });
}

function resetCameraForNextCapture(message = "已确认放入，可继续拍摄下一份食材。") {
  cameraCapturedFood = null;
  cameraRegistrationNote = "";
  cameraResult.classList.remove("is-ready");
  cameraFoodName.textContent = "等待下一次拍摄";
  cameraFoodMeta.textContent = message;
  shutterButton.disabled = false;
  cameraConfirmButton.disabled = true;
  manualCapturePreview.textContent = "未输入补充信息";
  cameraVoiceStatus.textContent = "可补充位置、数量、备注。";
}

function confirmCameraFoodPlaced() {
  if (!cameraCapturedFood) {
    return;
  }
  const savedRecord = writeCameraFoodToInventory(cameraCapturedFood);
  const savedPlace = savedRecord?.place || "冰箱宫格";
  resetCameraForNextCapture(`已登记 ${cameraCapturedFood.name} · ${savedPlace} · ${savedRecord?.amount || cameraCapturedFood.amount}。`);
  setPage("camera");
}

function applyCameraEditText(text) {
  if (!cameraCapturedFood) {
    captureFoodFromCamera();
  }
  const rules = [
    [/名称(?:改成|是|为)?(.+)/u, "name"],
    [/食材(?:改成|是|为)?(.+)/u, "name"],
    [/数量(?:改成|是|为)?(.+)/u, "amount"],
    [/位置(?:改成|是|为)?(.+)/u, "place"],
    [/备注(?:改成|是|为)?(.+)/u, "note"],
  ];
  const matched = rules.find(([pattern]) => pattern.test(text));
  if (matched) {
    const [pattern, key] = matched;
    cameraCapturedFood[key] = text.match(pattern)[1].trim();
    if (key === "name") {
      const recommended = recommendFoodPlacement(cameraCapturedFood.name);
      cameraCapturedFood.place = recommended.place;
      cameraCapturedFood.placementReason = recommended.reason;
    }
    cameraRegistrationNote = cameraCapturedFood.note;
    manualCapturePreview.textContent = cameraCapturedFood.note;
    updateCameraResult();
    cameraVoiceStatus.textContent = `已更新识别结果：${text}`;
    return;
  }
  appendCameraRegistrationNote(text);
  cameraVoiceStatus.textContent = `已补充到本次识别：${text}`;
}

function applyVoiceText(text, mode) {
  const cleanText = text.trim().replace(/[。.!！]$/u, "");
  if (!cleanText) return;

  if (mode === "cameraNote") {
    applyCameraEditText(cleanText);
    return;
  }

  if (mode === "note") {
    appendCurrentFoodNote(cleanText);
    voiceStatus.textContent = `已写入备注：${cleanText}`;
    return;
  }

  const rules = [
    [/名称(?:改成|是|为)?(.+)/u, foodNameInput],
    [/食材(?:改成|是|为)?(.+)/u, foodNameInput],
    [/数量(?:改成|是|为)?(.+)/u, foodAmountInput],
    [/到期(?:时间)?(?:改成|是|为)?(.+)/u, foodExpireInput],
    [/位置(?:改成|是|为)?(.+)/u, foodPlaceInput],
    [/备注(?:改成|是|为)?(.+)/u, foodNoteInput],
  ];

  const matched = rules.find(([pattern]) => pattern.test(cleanText));
  if (matched) {
    const [pattern, input] = matched;
    input.value = cleanText.match(pattern)[1].trim();
    voiceStatus.textContent = `已识别并更新：${cleanText}`;
  } else {
    appendCurrentFoodNote(cleanText);
    voiceStatus.textContent = `未识别字段，已追加到备注：${cleanText}`;
  }
}

function startVoiceInput(mode, button) {
  const SpeechRecognition = window.SpeechRecognition || window.webkitSpeechRecognition;
  const statusNode = mode === "cameraNote" ? cameraVoiceStatus : voiceStatus;

  if (!SpeechRecognition) {
    const mockText = mode === "smart" ? "数量改成 3 个" : "今天优先使用，外皮较软";
    applyVoiceText(mockText, mode);
    statusNode.textContent = `当前浏览器不支持语音识别，已模拟：${mockText}`;
    return;
  }

  const recognition = new SpeechRecognition();
  recognition.lang = "zh-CN";
  recognition.interimResults = false;
  recognition.maxAlternatives = 1;

  button.classList.add("is-listening");
  statusNode.textContent = "正在聆听...";

  recognition.onresult = (event) => {
    const text = event.results[0][0].transcript;
    applyVoiceText(text, mode);
  };

  recognition.onerror = () => {
    statusNode.textContent = "没有听清，可以再试一次。";
  };

  recognition.onend = () => {
    button.classList.remove("is-listening");
  };

  recognition.start();
}

function updateManualCaptureText() {
  const placeholders = {
    edit: "请输入数量、保质期或备注",
    wifi: "请输入 Wi-Fi 密码",
    camera: "请输入位置、数量或备注",
  };
  const placeholder = placeholders[manualInputTarget] || placeholders.camera;
  const composing = pinyinBuffer ? `  拼音：${pinyinBuffer}` : "";
  manualInputLine.textContent = manualCaptureText ? `${manualCaptureText}${composing}` : (pinyinBuffer ? `拼音：${pinyinBuffer}` : placeholder);
  if (manualInputTarget === "wifi") {
    wifiPasswordInput.value = manualCaptureText;
  }
}

function updateKeyboardModeLabel() {
  const labels = {
    letters: keyboardLanguage === "zh" ? "中文拼音" : (keyboardCase === "upper" ? "英文大写" : "英文小写"),
    numbers: "数字输入",
    symbols: "符号输入",
  };
  keyboardModeLabel.textContent = labels[keyboardPanel] || labels.letters;
}

function openCaptureKeyboard(target = "camera") {
  manualInputTarget = target;
  manualCaptureText = target === "wifi" ? wifiPasswordInput.value : "";
  pinyinBuffer = "";
  keyboardLanguage = "zh";
  keyboardCase = "upper";
  keyboardPanel = "letters";
  const keyboardTitles = {
    edit: "手动补充菜品信息",
    wifi: "输入 Wi-Fi 密码",
    camera: "手动补充登记信息",
  };
  manualKeyboardTitle.textContent = keyboardTitles[target] || keyboardTitles.camera;
  screen.classList.add("capture-keyboard-open");
  captureKeyboard.classList.add("is-open");
  buildCaptureKeyboard();
  updateManualCaptureText();
}

function closeCaptureKeyboard() {
  screen.classList.remove("capture-keyboard-open");
  captureKeyboard.classList.remove("is-open");
}

function confirmManualCapture() {
  const text = manualCaptureText.trim();
  if (!text) {
    if (manualInputTarget === "edit") {
      voiceStatus.textContent = "尚未输入补充信息。";
    } else if (manualInputTarget === "wifi") {
      wifiDialogStatus.textContent = "尚未输入 Wi-Fi 密码。";
    } else {
      cameraVoiceStatus.textContent = "尚未输入补充信息。";
    }
    closeCaptureKeyboard();
    return;
  }
  if (manualInputTarget === "edit") {
    manualEditPreview.textContent = text;
    voiceStatus.textContent = `已手动补充：${text}`;
    appendCurrentFoodNote(text);
  } else if (manualInputTarget === "wifi") {
    wifiPasswordInput.value = text;
    wifiDialogStatus.textContent = "密码已输入，可点击连接。";
  } else {
    if (!cameraCapturedFood) {
      captureFoodFromCamera();
    }
    appendCameraRegistrationNote(text);
    cameraVoiceStatus.textContent = `已手动补充到本次登记：${text}`;
  }
  closeCaptureKeyboard();
}

function addKeyboardKey(label, className, handler, text = label) {
  const button = document.createElement("button");
  button.type = "button";
  button.className = `capture-key ${className || ""}`.trim();
  button.textContent = label;
  button.setAttribute("aria-label", text);
  button.addEventListener("click", handler);
  captureKeyboardKeys.appendChild(button);
}

function appendManualText(value) {
  manualCaptureText += value;
  pinyinBuffer = "";
  updateManualCaptureText();
}

function setKeyboardPanel(panel) {
  if (panel !== "letters") {
    pinyinBuffer = "";
  }
  keyboardPanel = panel;
  buildCaptureKeyboard();
  updateManualCaptureText();
}

function toggleKeyboardLanguage() {
  keyboardLanguage = keyboardLanguage === "zh" ? "en" : "zh";
  pinyinBuffer = "";
  keyboardPanel = "letters";
  buildCaptureKeyboard();
  updateManualCaptureText();
}

function toggleKeyboardCase() {
  keyboardCase = keyboardCase === "upper" ? "lower" : "upper";
  keyboardLanguage = "en";
  pinyinBuffer = "";
  keyboardPanel = "letters";
  buildCaptureKeyboard();
  updateManualCaptureText();
}

function getLetterKeys() {
  const letters = "QWERTYUIOPASDFGHJKLZXCVBNM".split("");
  return keyboardCase === "lower" || keyboardLanguage === "zh" ? letters.map((letter) => letter.toLowerCase()) : letters;
}

function getNumberKeys() {
  return ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "个", "盒", "袋", "瓶", "斤", "g", "ml", "天", "月", "日"];
}

function getSymbolKeys() {
  return ["，", "。", "、", "：", "；", "！", "？", "（", "）", "-", "/", "+", "=", "%", "#", "@", ".", ",", ":", ";"];
}

function renderCharacterKeys(keys) {
  keys.forEach((key) => {
    addKeyboardKey(key, "", () => handleKeyboardInput(key));
  });
}

function handleKeyboardInput(key) {
  if (/^[a-z]$/.test(key)) {
    appendManualText(key);
    return;
  }
  appendManualText(key);
}

function renderKeyboardTools() {
  addKeyboardKey(keyboardLanguage === "zh" ? "中/英" : "En/中", "wide tool", toggleKeyboardLanguage, "切换中英文");
  addKeyboardKey(keyboardCase === "upper" ? "A/a" : "a/A", "wide tool", toggleKeyboardCase, "切换大小写");
  addKeyboardKey(keyboardPanel === "numbers" ? "拼音/ABC" : "123", "wide tool", () => setKeyboardPanel(keyboardPanel === "numbers" ? "letters" : "numbers"), "切换数字");
  addKeyboardKey(keyboardPanel === "symbols" ? "拼音/ABC" : "#+=", "wide tool", () => setKeyboardPanel(keyboardPanel === "symbols" ? "letters" : "symbols"), "切换符号");
  addKeyboardKey("空格", "space", () => appendManualText(" "));
  addKeyboardKey("删除", "wide", () => {
    manualCaptureText = manualCaptureText.slice(0, -1);
    updateManualCaptureText();
  });
  addKeyboardKey("清空", "wide", () => {
    manualCaptureText = "";
    pinyinBuffer = "";
    buildCaptureKeyboard();
    updateManualCaptureText();
  });
  addKeyboardKey("确认", "wide action", confirmManualCapture);
}

function buildCaptureKeyboard() {
  captureKeyboardKeys.innerHTML = "";
  updateKeyboardModeLabel();
  const panelKeys = {
    letters: getLetterKeys(),
    numbers: getNumberKeys(),
    symbols: getSymbolKeys(),
  };
  renderCharacterKeys(panelKeys[keyboardPanel] || panelKeys.letters);
  renderKeyboardTools();
}

function rotateStandbyFace(animated = true) {
  if (screen.classList.contains("is-screen-off")) {
    return;
  }
  const [face, mood] = standbyFaces[Math.floor(Math.random() * standbyFaces.length)];
  if (!animated) {
    mascotFace.textContent = face;
    standbyMood.textContent = mood;
    return;
  }
  mascot.classList.add("is-changing");
  window.setTimeout(() => {
    mascotFace.textContent = face;
    standbyMood.textContent = mood;
    mascot.classList.remove("is-changing");
  }, 180);
}

function enterIdleStandby() {
  window.clearInterval(demoTimer);
  closeCaptureKeyboard();
  if (wifiModal.classList.contains("is-open")) {
    closeWifiDialog();
  }
  setPage("standby");
}

function resetInactivityTimer() {
  window.clearTimeout(inactivityTimer);
  inactivityTimer = window.setTimeout(enterIdleStandby, getInactivityTimeout());
}

function handleUserActivity(event) {
  if (event?.target?.closest?.(".controls")) {
    resetInactivityTimer();
    return;
  }
  if (screen.classList.contains("is-screen-off") || currentPageName === "standby") {
    setPage("home");
  }
  resetInactivityTimer();
}

function returnFromStatusBar() {
  if (isChoosingFoodPlace) {
    isChoosingFoodPlace = false;
    setPage("editFood");
    return;
  }
  const targetPage = pageReturnTargets[currentPageName];
  if (targetPage) {
    if (currentPageName === "editFood") {
      renderZone(currentZone);
    }
    if (currentPageName === "cameraResult") {
      resetCameraForNextCapture();
    }
    setPage(targetPage);
  }
}

function setSpaceEditing(enabled) {
  isSpaceEditing = enabled;
  screen.classList.toggle("is-space-editing", enabled);
  spaceEditToggle.textContent = enabled ? "完成编辑" : "编辑空间";
  if (spaceEditor) {
    spaceEditor.classList.toggle("is-open", enabled);
  }
  if (enabled && !activeCustomZoneButton) {
    syncSpaceEditorFromButton(document.querySelector(".fridge-map .zone:not(.zone-add)"));
  }
  if (!enabled) {
    document.querySelectorAll(".fridge-map .zone").forEach((zone) => {
      zone.classList.remove("is-edit-target");
    });
  }
}

function syncSpaceEditorFromButton(button) {
  activeCustomZoneButton = button;
  if (!button) return;
  const span = button.querySelector("span");
  const note = button.dataset.note || button.querySelector("strong")?.textContent || "可放食材";
  spaceNameInput.value = span?.textContent || "自定义区";
  spaceWidthRange.value = button.dataset.width || "1";
  spaceHeightRange.value = button.dataset.height || "1";
  spaceNoteInput.value = note;
  document.querySelectorAll(".fridge-map .zone").forEach((zone) => {
    zone.classList.toggle("is-edit-target", zone === button);
  });
}

function applySpaceEditorToButton() {
  if (!activeCustomZoneButton) return;
  const name = spaceNameInput.value.trim() || "自定义区";
  const note = spaceNoteInput.value.trim() || "可放食材";
  const width = spaceWidthRange.value;
  const height = spaceHeightRange.value;
  const key = activeCustomZoneButton.dataset.zone;

  activeCustomZoneButton.dataset.width = width;
  activeCustomZoneButton.dataset.height = height;
  activeCustomZoneButton.dataset.note = note;
  activeCustomZoneButton.style.gridColumn = `span ${width}`;
  activeCustomZoneButton.style.gridRow = `span ${height}`;
  activeCustomZoneButton.querySelector("span").textContent = name;
  activeCustomZoneButton.querySelector("strong").textContent = note;
  if (zoneNames[key]) {
    zoneNames[key] = name;
  }
}

function getRandomSpaceColor() {
  const theme = document.body.dataset.theme || "warm";
  const palette = spacePalettes[theme] || spacePalettes.warm;
  return palette[Math.floor(Math.random() * palette.length)];
}

function deleteActiveSpace() {
  if (!activeCustomZoneButton) return;
  const key = activeCustomZoneButton.dataset.zone;
  activeCustomZoneButton.remove();
  delete zoneNames[key];
  delete zoneFoods[key];
  Object.keys(foodRecords).forEach((recordKey) => {
    if (recordKey.startsWith(`${key}:`)) {
      delete foodRecords[recordKey];
    }
  });
  if (currentZone === key) {
    currentZone = Object.keys(zoneFoods)[0] || "";
    currentSlot = "A1";
  }
  activeCustomZoneButton = document.querySelector(".fridge-map .zone:not(.zone-add)");
  if (activeCustomZoneButton) {
    syncSpaceEditorFromButton(activeCustomZoneButton);
  } else {
    spaceNameInput.value = "自定义区";
    spaceWidthRange.value = "1";
    spaceHeightRange.value = "1";
    spaceNoteInput.value = "可放食材";
  }
  if (!document.querySelector(".zone-custom")) {
    screen.classList.remove("has-custom-zones");
  }
}

function addCustomFridgeZone() {
  customZoneCount += 1;
  const key = `custom${customZoneCount}`;
  const label = `自定义区 ${customZoneCount}`;
  zoneNames[key] = label;
  zoneFoods[key] = [
    ["A1", "空位", "可放食材"],
    ["A2", "空位", "可放食材"],
    ["A3", "空位", "可放食材"],
    ["B1", "空位", "可放食材"],
    ["B2", "空位", "可放食材"],
    ["B3", "空位", "可放食材"],
    ["C1", "空位", "可放食材"],
    ["C2", "空位", "可放食材"],
    ["C3", "空位", "可放食材"],
  ];

  const button = document.createElement("button");
  button.className = "zone zone-custom";
  button.type = "button";
  button.dataset.zone = key;
  button.dataset.width = "1";
  button.dataset.height = "1";
  button.dataset.note = "可放食材";
  button.style.background = getRandomSpaceColor();
  button.innerHTML = `<span>${label}</span><strong>可放食材</strong>`;
  screen.classList.add("has-custom-zones");
  button.addEventListener("click", () => {
    if (isSpaceEditing) {
      syncSpaceEditorFromButton(button);
      return;
    }
    window.clearInterval(demoTimer);
    currentSlot = "A1";
    renderZone(key);
    setPage("zone");
  });
  addFridgeZone.before(button);
  syncSpaceEditorFromButton(button);
}

function openFoodEditor(slot, food, note) {
  currentSlot = slot;
  currentFoodKey = getFoodKey(currentZone, slot);
  editingFoodSource = { key: currentFoodKey, zone: currentZone, slot };
  if (!foodRecords[currentFoodKey]) {
    foodRecords[currentFoodKey] = buildFoodRecord(currentZone, slot, food, note);
  }
  const record = foodRecords[currentFoodKey];
  record.place = normalizeDisplayPlace(record.place, currentZone, slot);
  foodNameInput.value = record.name;
  foodAmountInput.value = record.amount;
  foodExpireInput.value = record.expire;
  foodPlaceInput.value = getDisplayPlace(currentZone, slot);
  foodStoredInput.value = formatDateTime(record.storedAt);
  foodNoteInput.value = record.note;
  manualEditPreview.textContent = "未输入补充信息";
  voiceStatus.textContent = "可说：“数量改成 3 个”“备注 今天先吃”。";
  setPage("editFood");
}

function beginFoodPlaceSelection() {
  isChoosingFoodPlace = true;
  setSpaceEditing(false);
  closeCaptureKeyboard();
  statusPanel.classList.remove("is-open");
  setPage("home");
}

function collectFoodFormRecord(zone, slot) {
  const storedValue = foodStoredInput.value.trim();
  const parsedStoredAt = new Date(storedValue.replace(" ", "T"));
  return {
    name: foodNameInput.value.trim(),
    amount: foodAmountInput.value.trim() || "待确认",
    expire: foodExpireInput.value.trim() || "待设置",
    place: getDisplayPlace(zone, slot),
    storedAt: Number.isNaN(parsedStoredAt.getTime()) ? new Date().toISOString() : parsedStoredAt.toISOString(),
    note: foodNoteInput.value.trim(),
  };
}

function updateNoteForPlaceChange(note, oldPlace, newPlace) {
  const cleanNote = note || "";
  const replacedNote = replacePlaceInText(cleanNote, oldPlace, newPlace);
  if (replacedNote !== cleanNote) {
    return replacedNote;
  }
  const prefix = cleanNote.trim() ? "\n" : "";
  return `${cleanNote}${prefix}位置已调整为${newPlace}。`;
}

function clearZoneFoodSlot(zone, slot) {
  const zoneItems = zoneFoods[zone];
  if (!zoneItems) return;
  const itemIndex = zoneItems.findIndex(([itemSlot]) => itemSlot === slot);
  if (itemIndex >= 0) {
    zoneItems[itemIndex] = [slot, "空位", "待补货"];
  }
}

function applyFoodPlaceSelection(zone, slot) {
  const nextKey = getFoodKey(zone, slot);
  const oldPlace = foodPlaceInput.value.trim();
  const record = collectFoodFormRecord(zone, slot);
  record.note = updateNoteForPlaceChange(record.note, oldPlace, record.place);
  if (editingFoodSource.key && editingFoodSource.key !== nextKey) {
    delete foodRecords[editingFoodSource.key];
    clearZoneFoodSlot(editingFoodSource.zone, editingFoodSource.slot);
  }
  foodRecords[nextKey] = record;

  const zoneItems = zoneFoods[zone];
  const itemIndex = zoneItems.findIndex(([itemSlot]) => itemSlot === slot);
  if (itemIndex >= 0) {
    zoneItems[itemIndex] = [slot, record.name || "空位", record.amount];
  }

  currentZone = zone;
  currentSlot = slot;
  currentFoodKey = nextKey;
  editingFoodSource = { key: nextKey, zone, slot };
  foodPlaceInput.value = record.place;
  foodNoteInput.value = record.note;
  isChoosingFoodPlace = false;
  updateFridgeOverviewCounts();
  renderZone(zone);
  renderExpiringFoods();
  setPage("editFood");
}

function openFoodEditorAt(zone, slot) {
  currentZone = zone;
  currentSlot = slot;
  renderZone(zone);
  const item = zoneFoods[zone]?.find(([itemSlot]) => itemSlot === slot);
  if (!item) return;
  openFoodEditor(item[0], item[1], item[2]);
}

function renderExpiringFoods() {
  if (!expiringFoodList) return;
  expiringFoodList.innerHTML = "";
  let visibleCount = 0;

  expiringFoods.forEach(({ zone, slot, marker, status }) => {
    const record = foodRecords[getFoodKey(zone, slot)];
    if (!record) return;
    visibleCount += 1;

    const article = document.createElement("article");
    const mark = document.createElement("div");
    const content = document.createElement("div");
    const title = document.createElement("strong");
    const meta = document.createElement("span");
    const editButton = document.createElement("button");

    mark.className = `food-mark ${marker}`;
    title.textContent = `${record.name || "空位"} · ${record.amount || "待确认"}`;
    meta.textContent = `${getDisplayPlace(zone, slot)} · ${status}`;
    editButton.type = "button";
    editButton.textContent = "编辑";
    editButton.addEventListener("click", () => openFoodEditorAt(zone, slot));

    content.append(title, meta);
    article.append(mark, content, editButton);
    expiringFoodList.appendChild(article);
  });

  if (expiringCount) {
    expiringCount.textContent = String(visibleCount);
  }
}

function saveCurrentFood() {
  const record = foodRecords[currentFoodKey];
  if (!record) return;

  Object.assign(record, collectFoodFormRecord(currentZone, currentSlot));

  const zoneItems = zoneFoods[currentZone];
  const itemIndex = zoneItems.findIndex(([slot]) => slot === currentSlot);
  if (itemIndex >= 0) {
    zoneItems[itemIndex] = [currentSlot, record.name || "空位", record.amount];
  }
  updateFridgeOverviewCounts();
  renderZone(currentZone);
  renderExpiringFoods();
}

function renderZone(zone) {
  currentZone = zone;
  zoneTitle.textContent = zoneNames[zone];
  gridBoard.innerHTML = "";

  zoneFoods[zone].forEach(([slot, food, note]) => {
    const recordKey = getFoodKey(zone, slot);
    if (!foodRecords[recordKey]) {
      foodRecords[recordKey] = buildFoodRecord(zone, slot, food, note);
    }
    const record = foodRecords[recordKey];
    const button = document.createElement("button");
    button.className = `slot${slot === currentSlot ? " is-selected" : ""}`;
    button.type = "button";
    button.innerHTML = `
      <span class="slot-code">${getSlotDisplayName(slot)}</span>
      <div class="slot-main">
        <strong class="slot-food">${record.name || "空位"}</strong>
        <strong class="slot-amount">${record.amount || note}</strong>
      </div>
      <span class="slot-expire">到期 ${record.expire || "待设置"}</span>
    `;
    button.addEventListener("click", () => {
      if (isChoosingFoodPlace) {
        applyFoodPlaceSelection(zone, slot);
        return;
      }
      openFoodEditor(slot, record.name || food, record.amount || note);
    });
    gridBoard.appendChild(button);
  });

  const selected = zoneFoods[zone].find(([slot]) => slot === currentSlot) || zoneFoods[zone][0];
  currentSlot = selected[0];
}

navButtons.forEach((button) => {
  button.addEventListener("click", () => {
    window.clearInterval(demoTimer);
    if (button.dataset.pageTarget === "editFood" && button.dataset.editZone && button.dataset.editSlot) {
      openFoodEditorAt(button.dataset.editZone, button.dataset.editSlot);
      return;
    }
    if (button === saveFoodButton) {
      saveCurrentFood();
    }
    setPage(button.dataset.pageTarget);
  });
});

zoneButtons.forEach((button) => {
  button.addEventListener("click", () => {
    if (isSpaceEditing) {
      if (!zoneFoods[button.dataset.zone]) return;
      syncSpaceEditorFromButton(button);
      return;
    }
    if (!zoneFoods[button.dataset.zone]) return;
    window.clearInterval(demoTimer);
    currentSlot = isChoosingFoodPlace ? (zoneFoods[button.dataset.zone]?.[0]?.[0] || "A1") : "B2";
    renderZone(button.dataset.zone);
    setPage("zone");
  });
});

demoRun.addEventListener("click", () => {
  const flow = ["standby", "home", "zone", "camera", "editFood", "settings", "more"];
  let index = 0;

  window.clearInterval(demoTimer);
  renderZone("left");
  setPage(flow[index]);

  demoTimer = window.setInterval(() => {
    index = (index + 1) % flow.length;
    if (flow[index] === "zone") {
      renderZone(index % 2 === 0 ? "left" : "door");
    }
    setPage(flow[index]);
  }, 1800);
});

statusBack.addEventListener("click", (event) => {
  event.stopPropagation();
  returnFromStatusBar();
});

statusBar.addEventListener("click", (event) => {
  if (event.target.closest("#statusBack")) return;
  statusPanel.classList.toggle("is-open");
});

statusBar.addEventListener("keydown", (event) => {
  if (event.key === "Enter" || event.key === " ") {
    event.preventDefault();
    statusPanel.classList.toggle("is-open");
  }
});

spaceEditToggle.addEventListener("click", () => {
  setSpaceEditing(!isSpaceEditing);
});

addFridgeZone.addEventListener("click", () => {
  addCustomFridgeZone();
});

[spaceNameInput, spaceWidthRange, spaceHeightRange, spaceNoteInput].forEach((input) => {
  input.addEventListener("input", applySpaceEditorToButton);
});

spaceDeleteButton.addEventListener("click", deleteActiveSpace);
shutterButton.addEventListener("click", captureFoodFromCamera);
cameraConfirmButton.addEventListener("click", confirmCameraFoodPlaced);
foodPlaceEditButton.addEventListener("click", beginFoodPlaceSelection);

brightnessRange.addEventListener("input", (event) => {
  setBrightness(event.target.value);
});

[statusWifiSelect, settingsWifiSelect].filter(Boolean).forEach((select) => {
  select.addEventListener("change", (event) => {
    handleWifiSelection(event.target.value);
  });
});

wifiCancel.addEventListener("click", closeWifiDialog);
wifiConnect.addEventListener("click", connectWifi);
wifiPasswordInput.addEventListener("keydown", (event) => {
  if (event.key === "Enter") {
    connectWifi();
  }
});
wifiPasswordInput.addEventListener("focus", () => openCaptureKeyboard("wifi"));
wifiPasswordInput.addEventListener("click", () => openCaptureKeyboard("wifi"));
wifiModal.addEventListener("click", (event) => {
  if (event.target === wifiModal) {
    closeWifiDialog();
  }
});

["pointerdown", "touchstart", "keydown"].forEach((eventName) => {
  document.addEventListener(eventName, handleUserActivity, { passive: true });
});

voiceButtons.forEach((button) => {
  button.addEventListener("click", () => {
    startVoiceInput(button.dataset.voiceTarget, button);
  });
});

manualCaptureOpen.addEventListener("click", () => openCaptureKeyboard("camera"));
manualEditOpen.addEventListener("click", () => openCaptureKeyboard("edit"));
manualCaptureClose.addEventListener("click", closeCaptureKeyboard);

const params = new URLSearchParams(window.location.search);
const initialZone = params.get("zone") || "left";
const initialPage = params.get("page") || "standby";
const initialTheme = "warm";
const initialBrightness = params.get("brightness") || localStorage.getItem("fridgeBrightness") || "100";
const initialWifi = params.get("wifi") || localStorage.getItem("fridgeWifi") || "Home_2.4G";

setTheme(initialTheme);
setBrightness(initialBrightness);
syncWifiSelection(initialWifi);
buildCaptureKeyboard();
renderZone(zoneFoods[initialZone] ? initialZone : "left");
updateFridgeOverviewCounts();
renderExpiringFoods();
setPage(pageMeta[initialPage] ? initialPage : "standby");
if (params.get("space") === "edit") {
  setSpaceEditing(true);
}
if (params.get("status") === "open" && initialPage !== "standby") {
  statusPanel.classList.add("is-open");
}
if (params.get("manual") === "open" && initialPage === "camera") {
  openCaptureKeyboard("camera");
}
if (params.get("manual") === "open" && initialPage === "editFood") {
  openCaptureKeyboard("edit");
}
rotateStandbyFace(false);
updateClockDisplay();
mascotTimer = window.setInterval(rotateStandbyFace, 2600);
window.setInterval(updateClockDisplay, 1000);
themeTimer = window.setInterval(refreshAutoTheme, 60000);
powerTimer = window.setInterval(updateStandbyPowerMode, 60000);
resetInactivityTimer();
