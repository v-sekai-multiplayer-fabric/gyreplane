import { test, expect } from '@playwright/test';

/*
 * Real end-to-end test against mud/web/index.html + mud.js, driven at
 * the actual DOM the real page produces (log/#commandForm/#commandInput
 * from index.html, .turn/.meta divs mud.js's own appendTurn() creates)
 * -- not a mock of the page.
 *
 * Red first, on purpose (red-green-refactor): written before task #33's
 * Fly deploy is confirmed reachable end to end, so a failing run here
 * is the expected, correct state right now. Green means a real command
 * round-tripped through POST /api/mud/command against a real running
 * zone-server-h2o + mud-sandbox-orchestrator + guest ELF, not that the
 * test itself was loosened to pass.
 *
 * MUD_BASE_URL must point at a real reachable instance (this session's
 * own Fly test app, while it exists, or a future stable deployment).
 * No default -- a missing env var should fail loudly, not silently
 * test nothing.
 *
 * Real finding, worth recording: against an IPv6-only host (this
 * project's own Fly deployments are IPv6-only by design, see
 * fly/fly.toml's own comment), Chromium's built-in DNS resolver can
 * fail to resolve the hostname even when curl/Node's plain http
 * client resolve it instantly in the same environment -- confirmed
 * directly, not assumed, including with --no-sandbox to rule out a
 * sandbox network-namespace cause. Navigating to the bracketed IPv6
 * literal (e.g. http://[2a09:...]/) instead of the hostname works
 * around it. If this test times out on page.goto() against a real,
 * curl-reachable IPv6-only host, try the literal address first before
 * assuming the deployment itself is broken.
 */

const baseURL = process.env.MUD_BASE_URL;

test.beforeAll(() => {
  if (!baseURL) {
    throw new Error('MUD_BASE_URL is not set -- point it at a real running instance');
  }
});

test('loads the Middleham page and shows the initial connected line', async ({ page }) => {
  await page.goto(baseURL!);
  await expect(page.locator('#log .turn').first()).toContainText('Connected.');
});

test('a real "look" command round-trips through the real backend', async ({ page }) => {
  await page.goto(baseURL!);
  await page.locator('#commandInput').fill('look');
  await page.locator('#commandForm button[type="submit"]').click();

  // The real narration text mud_guest.cpp's own MiddlehamStateMachine
  // produces for city_gate's look command (state_machine.py's own
  // _ROOM_TEMPLATES["city_gate"], ported verbatim) -- not a generic
  // substring, the actual expected room description.
  const lastTurn = page.locator('#log .turn').last();
  await expect(lastTurn).toContainText('Middleham City Gate', { timeout: 10000 });
  await expect(lastTurn.locator('.meta')).toContainText('turn 1');
});
