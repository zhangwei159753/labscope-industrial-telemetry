import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],
  build: {
    rollupOptions: {
      output: {
        manualChunks(id) {
          if (id.includes('/node_modules/zrender/')) return 'zrender'
          if (id.includes('/node_modules/echarts/')) return 'echarts'
        },
      },
    },
  },
  server: {
    proxy: {
      '/api': 'http://127.0.0.1:8848',
      '/healthz': 'http://127.0.0.1:8848',
      '/ws': { target: 'ws://127.0.0.1:8848', ws: true },
    },
  },
})
