const api = require('../../utils/api');
const { loadConfig, updateConfig } = require('../../utils/storage');

Page({
  data: {
    sttPath: '/stt',
    drawPath: '/draw',
    imageThreshold: 210,
    printEnabled: false,
    baseUrl: '',
  },

  onLoad() {
    const config = loadConfig();
    this.setData({
      sttPath: config.advanced.sttPath,
      drawPath: config.advanced.drawPath,
      imageThreshold: config.advanced.imageThreshold,
      printEnabled: config.advanced.printEnabled,
      baseUrl: config.backend.baseUrl,
    });
  },

  onInput(event) {
    this.setData({
      [event.currentTarget.dataset.key]: event.detail.value,
    });
  },

  onThresholdChange(event) {
    this.setData({ imageThreshold: event.detail.value });
  },

  onPrintChange(event) {
    this.setData({ printEnabled: event.detail.value });
  },

  saveLocal() {
    updateConfig({
      advanced: {
        sttPath: this.data.sttPath,
        drawPath: this.data.drawPath,
        imageThreshold: this.data.imageThreshold,
        printEnabled: this.data.printEnabled,
      },
    });
    wx.showToast({ title: '已保存', icon: 'success' });
  },

  async pushBackend() {
    this.saveLocal();
    try {
      await api.updateBackendConfig({
        image_threshold: this.data.imageThreshold,
        print_enabled: this.data.printEnabled,
      }, this.data.baseUrl);
      wx.showToast({ title: '已同步', icon: 'success' });
    } catch (error) {
      wx.showToast({ title: '后端接口还没开', icon: 'none' });
    }
  },
});
