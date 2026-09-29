const { PRESETS } = await import((process.env.VS ?? '/home/claude/videoskillet') + '/src/ui/presets.ts')
console.log(PRESETS.map(p => p.name).join('\n'))
