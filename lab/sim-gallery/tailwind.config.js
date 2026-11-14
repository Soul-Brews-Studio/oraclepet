/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      // Warm "amber cellar" palette — a developer's desk lit by a single CRT
      // and a kerosene flame. Not slate/zinc. Mostly black with intentional
      // warmth, so the amber Ψ reads as the heat source, not a brand color.
      colors: {
        ink:    '#06070a',     // page bg — true black, a hair warm
        coal:   '#0e0f12',     // section / panel bg
        dusk:   '#16171b',     // elevated surface, hover bg
        embers: '#23232a',     // hairline borders, dividers
        ash:    '#6f6f78',     // secondary text (warm gray, not slate-500)
        chalk:  '#e8e6df',     // primary text (warm white)

        // The flame
        glow:      '#f59e0b',  // amber primary — the Ψ
        'glow-soft': '#fbbf24', // hover / highlight
        wick:      '#fde68a',   // brightest accent (badges, links)
        ember:     '#b45309',   // deep amber (pressed)

        // Semantic accents (used sparingly, brewing-batch hints)
        moss:   '#5d8a5d',     // success / linked state
        rust:   '#c2410c',     // warning / pressed-tap
        oxide:  '#0f7b8a',     // info / clock

        // (kept for any leftover bindings)
        oracle: {
          flame: '#f59e0b',
          deep:  '#06070a',
          panel: '#0e0f12',
          edge:  '#23232a'
        }
      },
      fontFamily: {
        // Display: warm, hand-cut, gallery-opening serif (variable, hairline → bold)
        display: ['Fraunces', 'ui-serif', 'Georgia', 'serif'],
        // Body/UI sans — clean, readable, doesn't fight the serif
        sans:    ['Inter', 'ui-sans-serif', 'system-ui', 'sans-serif'],
        // Code, board names, badges, log
        mono:    ['"JetBrains Mono"', 'ui-monospace', 'SFMono-Regular', 'monospace']
      },
      fontSize: {
        // Tighter, more confident scale
        '2xs': ['10.5px', { lineHeight: '1.4' }],
        xs:    ['11.5px', { lineHeight: '1.45' }],
        sm:    ['13px',   { lineHeight: '1.55' }],
        base:  ['15px',   { lineHeight: '1.65' }],
        lg:    ['17px',   { lineHeight: '1.6' }],
        xl:    ['20px',   { lineHeight: '1.45' }],
        '2xl': ['26px',   { lineHeight: '1.3' }],
        '3xl': ['34px',   { lineHeight: '1.2' }],
        '4xl': ['44px',   { lineHeight: '1.1' }],
        '5xl': ['60px',   { lineHeight: '1.05', letterSpacing: '-0.02em' }],
        '6xl': ['76px',   { lineHeight: '1.0',  letterSpacing: '-0.025em' }]
      },
      letterSpacing: {
        tight2: '-0.015em',
        wider2: '0.16em'      // for tiny KICKER labels
      },
      boxShadow: {
        flame: '0 0 80px -20px rgba(245, 158, 11, 0.55)',
        card:  '0 20px 50px -30px rgba(0, 0, 0, 0.9)'
      },
      backgroundImage: {
        // The hero's kerosene flame, painted with a radial in CSS — no asset.
        'hero-flame':
          'radial-gradient(60% 50% at 18% 12%, rgba(245,158,11,0.18) 0%, rgba(245,158,11,0.06) 35%, rgba(0,0,0,0) 70%)',
        // Subtle vignette so the page feels held, not cropped.
        'page-vignette':
          'radial-gradient(120% 80% at 50% 0%, rgba(245,158,11,0.05) 0%, rgba(0,0,0,0) 55%)'
      }
    }
  },
  plugins: []
}
