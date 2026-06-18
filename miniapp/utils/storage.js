const { DEFAULT_CONFIG } = require('./constants');

const STORAGE_KEY = 'voice_ai_mic_config';

function isPlainObject(value) {
  return Object.prototype.toString.call(value) === '[object Object]';
}

function deepMerge(base, patch) {
  const output = { ...base };
  Object.keys(patch || {}).forEach((key) => {
    if (isPlainObject(base[key]) && isPlainObject(patch[key])) {
      output[key] = deepMerge(base[key], patch[key]);
    } else if (patch[key] !== undefined) {
      output[key] = patch[key];
    }
  });
  return output;
}

function loadConfig() {
  const stored = wx.getStorageSync(STORAGE_KEY) || {};
  return deepMerge(DEFAULT_CONFIG, stored);
}

function saveConfig(config) {
  const next = deepMerge(DEFAULT_CONFIG, config);
  wx.setStorageSync(STORAGE_KEY, next);
  const app = getApp({ allowDefault: true });
  if (app && app.globalData) {
    app.globalData.config = next;
  }
  return next;
}

function updateConfig(partial) {
  return saveConfig(deepMerge(loadConfig(), partial));
}

module.exports = {
  loadConfig,
  saveConfig,
  updateConfig,
};
