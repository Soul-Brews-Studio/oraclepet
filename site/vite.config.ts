import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// base './': the site is served under /oraclepet/ on GitHub Pages and from / locally.
// `npm run dev` serves ../_site (firmware/, packs/, ble/ from tools/site/build-site.sh)
// as the public dir, so the page finds firmware/index.json during development.
export default defineConfig(({ command }) => ({
  base: './',
  publicDir: command === 'serve' ? '../_site' : 'public',
  plugins: [react(), tailwindcss()],
  server: { host: '127.0.0.1' },
}))
