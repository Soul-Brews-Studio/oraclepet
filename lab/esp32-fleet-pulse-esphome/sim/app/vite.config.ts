import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// The Emscripten ES module (oracle_term_sim.mjs) + .wasm live in public/ and are
// loaded at runtime via a dynamic import — not bundled by Vite.
export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: { port: 5173, host: true },
})
