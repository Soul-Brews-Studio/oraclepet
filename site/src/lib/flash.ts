// USB flashing with esptool-js. Desktop Chrome/Edge: Web Serial. Chrome on Android has no
// Web Serial, so the same USB CDC port is opened over WebUSB (esptool-js WebUSBSerialPort).

import { ESPLoader, Transport, WebUSBSerialPort, ESPRESSIF_VID } from 'esptool-js'
import type { IEspLoaderTerminal } from 'esptool-js'

export type Api = 'serial' | 'usb' | null

export function usbApi(): Api {
  if ('serial' in navigator) return 'serial'
  if ('usb' in navigator) return 'usb'
  return null
}

export interface Image {
  url: string
  offset: number
}

export interface Progress {
  phase: 'connect' | 'download' | 'erase' | 'write' | 'reset' | 'done'
  part?: number
  parts?: number
  done: number // 0..1 overall
  message: string
}

interface Opts {
  images: Image[]
  eraseAll: boolean
  log: (line: string) => void
  progress: (p: Progress) => void
}

async function openPort(api: Api): Promise<SerialPort> {
  if (api === 'serial') {
    // 0x303a: Espressif's own USB (the S3's USB-Serial/JTAG); others are USB-UART bridges.
    return navigator.serial.requestPort()
  }
  if (api === 'usb') {
    const p = await WebUSBSerialPort.requestPort()
    return p.asSerialPort() as unknown as SerialPort
  }
  throw new Error('This browser cannot open a USB serial port. Use Chrome or Edge on desktop, or Chrome on Android.')
}

export async function flash({ images, eraseAll, log, progress }: Opts): Promise<string> {
  const api = usbApi()
  // requestPort() must be the first await after the click: the browser only shows the port
  // picker while the user gesture is still active, so the download comes after it.
  progress({ phase: 'connect', done: 0, message: 'Pick the board in the browser list' })
  const port = await openPort(api)
  progress({ phase: 'download', done: 0.01, message: 'Downloading firmware' })
  const blobs: Uint8Array[] = []
  for (const [i, img] of images.entries()) {
    const r = await fetch(img.url)
    if (!r.ok) throw new Error(`${img.url}: HTTP ${r.status}`)
    blobs.push(new Uint8Array(await r.arrayBuffer()))
    progress({ phase: 'download', done: (0.05 * (i + 1)) / images.length, message: `Downloaded ${i + 1} of ${images.length} files` })
  }

  const transport = new Transport(port, false)
  const terminal: IEspLoaderTerminal = {
    clean() {},
    write: (s) => log(s),
    writeLine: (s) => log(s),
  }
  const loader = new ESPLoader({ transport, baudrate: 921600, terminal })
  try {
    progress({ phase: 'connect', done: 0.06, message: 'Connecting to the chip' })
    const chip = await loader.main()
    log(`chip: ${chip}`)
    if (!/ESP32-S3/i.test(chip)) throw new Error(`This firmware is for the ESP32-S3; the board says ${chip}`)

    if (eraseAll) {
      progress({ phase: 'erase', done: 0.08, message: 'Erasing the whole flash (about 30 s)' })
      await loader.eraseFlash()
    }

    const total = blobs.reduce((n, b) => n + b.length, 0)
    const before = blobs.map((_, i) => blobs.slice(0, i).reduce((n, b) => n + b.length, 0))
    await loader.writeFlash({
      fileArray: blobs.map((data, i) => ({ data, address: images[i].offset })),
      flashMode: 'keep',
      flashFreq: 'keep',
      flashSize: 'keep',
      eraseAll: false,
      compress: true,
      reportProgress: (i, written, size) => {
        const done = (before[i] + (written / size) * blobs[i].length) / total
        progress({
          phase: 'write',
          part: i + 1,
          parts: blobs.length,
          done: 0.1 + 0.88 * done,
          message: `Writing ${i + 1} of ${blobs.length} · ${Math.round(done * 100)}%`,
        })
      },
    })

    progress({ phase: 'reset', done: 0.99, message: 'Restarting the board' })
    await loader.after('hard_reset')
    progress({ phase: 'done', done: 1, message: 'Done. The board is starting the new firmware.' })
    return chip
  } finally {
    await transport.disconnect().catch(() => {})
  }
}

export { ESPRESSIF_VID }
