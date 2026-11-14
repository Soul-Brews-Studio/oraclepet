import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// The LVGL sims emit pet_sim.js / oracle_face_sim_v1.js etc. that expect to be
// loaded as classic scripts and that grow their own WebAssembly memory. We don't
// want Vite to try to transform them.
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    fs: {
      // The pet sim character pack lives outside this app — let dev serve it.
      allow: ['..', '../..']
    }
  },
  build: {
    target: 'es2022',
    // Keep the emscripten outputs untouched in the final bundle
    assetsInlineLimit: 0
  }
})
