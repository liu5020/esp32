const ble = require('../../utils/ble');
const { loadConfig } = require('../../utils/storage');

Page({
  data: {
    config: loadConfig(),
    connected: false,
    devices: [],
    scanning: false,
  },

  onShow() {
    const app = getApp();
    this.setData({
      config: loadConfig(),
      connected: Boolean(app.globalData.bleConnection),
    });
  },

  async handleScan() {
    const config = loadConfig();
    this.setData({ scanning: true, devices: [] });
    try {
      const devices = await ble.scanDevices({
        timeoutMs: 4500,
        keyword: config.device.nameKeyword,
      });
      this.setData({ devices });
      if (!devices.length) {
        wx.showToast({ title: '没有发现设备', icon: 'none' });
      }
    } catch (error) {
      wx.showToast({ title: error.message || '扫描失败', icon: 'none' });
    } finally {
      this.setData({ scanning: false });
    }
  },

  async handleConnect(event) {
    const deviceId = event.currentTarget.dataset.deviceId;
    wx.showLoading({ title: '连接中' });
    try {
      const connection = await ble.connectDevice(deviceId);
      getApp().globalData.bleConnection = connection;
      this.setData({ connected: true });
      wx.showToast({ title: '已连接', icon: 'success' });
    } catch (error) {
      wx.showToast({ title: error.message || '连接失败', icon: 'none' });
    } finally {
      wx.hideLoading();
    }
  },

  go(event) {
    wx.navigateTo({ url: event.currentTarget.dataset.path });
  },
});
