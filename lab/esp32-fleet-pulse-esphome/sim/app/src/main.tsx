import { createRoot } from 'react-dom/client'
import App from './App'
import './index.css'

// No StrictMode — the Emscripten module owns a single canvas + global state and
// must not be created twice (StrictMode double-mounts effects in dev).
createRoot(document.getElementById('root')!).render(<App />)
