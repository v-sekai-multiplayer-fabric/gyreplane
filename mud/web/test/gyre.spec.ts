import { test, expect } from "./camoufox-fixtures";

/*
 * Real end-to-end test of the mode selector (RFD 0085,
 * multiplayer-fabric-manuals) added to mud/web/index.html + mud.js,
 * driven through Camoufox (a real Firefox-family browser, not
 * Chromium) at the actual DOM the real page produces.
 *
 * Same house rule mud.spec.ts already states: MUD_BASE_URL must point
 * at a real reachable zone-server-h2o + mud-sandbox-orchestrator +
 * the_gyre-domain guest ELF. No mock server, no default base URL --
 * red until that is real and reachable, matching this project's own
 * red-first-on-purpose convention.
 *
 * A throwaway local Node stub server (never committed) verified the
 * client-side logic itself -- mode switch, per-domain session id, the
 * `domain` field going out on the wire, real narration rendering --
 * under both Chromium and Camoufox before this spec was written. This
 * spec is the real thing that stub stood in for.
 */

const baseURL = process.env.MUD_BASE_URL;

test.beforeAll(() => {
  if (!baseURL) {
    throw new Error("MUD_BASE_URL is not set -- point it at a real running instance");
  }
});

test("switching the mode selector to The Gyre changes the page title", async ({ page }) => {
  await page.goto(baseURL!);
  await page.selectOption("#modeSelect", "the_gyre");
  await expect(page.locator("#title")).toHaveText("The Gyre");
});

test("a real Gyre loop: look, go east, both rooms visited completes the objective", async ({ page }) => {
  await page.goto(baseURL!);
  await page.selectOption("#modeSelect", "the_gyre");

  await page.locator("#commandInput").fill("look");
  await page.locator('#commandForm button[type="submit"]').click();

  // The real narration mud_guest.cpp's gyre_room_templates() produces
  // for decanting_floor's look command -- not a generic substring.
  let lastTurn = page.locator("#log .turn").last();
  await expect(lastTurn).toContainText("The Decanting Floor", { timeout: 10000 });

  await page.locator("#commandInput").fill("go east");
  await page.locator('#commandForm button[type="submit"]').click();

  lastTurn = page.locator("#log .turn").last();
  await expect(lastTurn).toContainText("The Splicer's Den", { timeout: 10000 });
  await expect(lastTurn.locator(".meta")).toContainText("objective complete");
});
