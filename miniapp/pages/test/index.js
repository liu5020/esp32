const api = require('../../utils/api');
const { loadConfig } = require('../../utils/storage');

function pretty(value) {
  return JSON.stringify(value, null, 2);
}

Page({
  data: {
    baseUrl: '',
    prompt: '画一只可爱的小猪',
    result: '',
    loadingHealth: false,
    loadingDraw: false,
  },

  onShow() {
    const config = loadConfig();
    this.setData({ baseUrl: config.backend.baseUrl });
  },

  onPromptInput(event) {
    this.setData({ prompt: event.detail.value });
  },

  async testHealth() {
    this.setData({ loadingHealth: true, result: '' });
    try {
      const data = await api.getHealth(this.data.baseUrl);
      this.setData({ result: pretty(data) });
    } catch (error) {
      this.setData({ result: pretty(error) });
    } finally {
      this.setData({ loadingHealth: false });
    }
  },

  async testDraw() {
    this.setData({ loadingDraw: true, result: '' });
    try {
      const data = await api.draw(this.data.prompt, this.data.baseUrl);
      this.setData({
        result: pretty({
          text: data.text,
          provider: data.provider || data.image_provider,
          image: data.image
            ? {
                width: data.image.width,
                height: data.image.height,
                format: data.image.format,
                bitmapChars: data.image.bitmap ? data.image.bitmap.length : 0,
              }
            : null,
        }),
      });
    } catch (error) {
      this.setData({ result: pretty(error) });
    } finally {
      this.setData({ loadingDraw: false });
    }
  },
});
