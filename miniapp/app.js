const { loadConfig } = require('./utils/storage');

App({
  globalData: {
    config: null,
    bleConnection: null,
  },

  onLaunch() {
    this.globalData.config = loadConfig();
  },
});
