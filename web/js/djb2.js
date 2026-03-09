/*
 * DJB2 hash — matches the C++ implementation in dj_plugin.cpp exactly.
 *
 * C++ uses `unsigned long` (64-bit on ARM64) with `%08lx` format.
 * We use BigInt to avoid JS number precision issues.
 */

export function djb2(str) {
    let hash = 5381n;
    for (let i = 0; i < str.length; i++) {
        hash = ((hash << 5n) + hash) + BigInt(str.charCodeAt(i));
    }
    return (hash & 0xFFFFFFFFFFFFFFFFn).toString(16).padStart(8, '0');
}
