const ble = require('../../utils/ble');
const { loadConfig, updateConfig } = require('../../utils/storage');

Page({
  data: {
    wifiSsid: '',
    wifiPassword: '',
    baseUrl: '',
  },

  onLoad() {
    const config = loadConfig();
    this.setData({
      wifiSsid: config.device.wifiSsid,
      wifiPassword: config.device.wifiPassword,
      baseUrl: config.backend.baseUrl,
    });
  },

  onInput(event) {
    this.setData({
      [event.currentTarget.dataset.key]: event.detail.value,
    });
  },

  saveLocal() {
    updateConfig({
      device: {
        wifiSsid: this.data.wifiSsid,
        wifiPassword: this.data.wifiPassword,
      },
      backend: {
        baseUrl: this.data.baseUrl,
      },
    });
    wx.showToast({ title: '已保存', icon: 'success' });
  },

  async sendToDevice() {
    this.saveLocal();
    const connection = getApp().globalData.bleConnection;
    try {
      await ble.writeJson(connection, {
        wifi: {
          ssid: this.data.wifiSsid,
          password: this.data.wifiPassword,
        },
        backend: {
          base_url: this.data.baseUrl,
        },
      });
      wx.showToast({ title: '已发送', icon: 'success' });
    } catch (error) {
      wx.showToast({ title: error.message || '发送失败', icon: 'none' });
    }
  },
});
