const { AI_MODES } = require('../../utils/constants');
const { loadConfig, updateConfig } = require('../../utils/storage');

function findMode(id) {
  return AI_MODES.find((mode) => mode.id === id) || AI_MODES[0];
}

Page({
  data: {
    modes: AI_MODES,
    aiMode: 'local_first',
    sttProvider: '',
    sttFallbackProvider: '',
    imageProvider: '',
  },

  onLoad() {
    const config = loadConfig();
    this.applyMode(config.backend.aiMode);
  },

  applyMode(modeId) {
    const mode = findMode(modeId);
    this.setData({
      aiMode: mode.id,
      sttProvider: mode.sttProvider,
      sttFallbackProvider: mode.sttFallbackProvider,
      imageProvider: mode.imageProvider,
    });
  },

  onModeChange(event) {
    this.applyMode(event.detail.value);
  },

  saveMode() {
    updateConfig({
      backend: {
        aiMode: this.data.aiMode,
        sttProvider: this.data.sttProvider,
        sttFallbackProvider: this.data.sttFallbackProvider,
        imageProvider: this.data.imageProvider,
      },
    });
    wx.showToast({ title: '已保存', icon: 'success' });
  },
});
