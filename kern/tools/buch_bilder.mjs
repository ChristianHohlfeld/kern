// Screenshots for the book (kern/docs/buch/bilder) at print resolution (device scale 2).
//
//   python3 -m http.server 8771 &          (from the repository root, after tools/build_studio.sh)
//   node kern/tools/buch_bilder.mjs http://127.0.0.1:8771 kern/docs/buch/bilder [names...]
//
// Software WebGL is slow: each picture waits for its scene. Without names it makes all of them.
import {createRequire} from "node:module";
import path from "node:path";
const {chromium} = await import("playwright").catch(() => createRequire(import.meta.url)("playwright"));
const [base, out, ...only] = process.argv.slice(2);
const want = name => only.length === 0 || only.includes(name);
const browser = await chromium.launch({args: ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"]});
const wait = ms => new Promise(r => setTimeout(r, ms));
const shot = async (page, name) => { await page.screenshot({path: path.join(out, name), timeout: 600000}); console.log(name); };

async function studio(viewport = {width: 1280, height: 800}, phone = false) {
  const ctx = await browser.newContext({viewport, deviceScaleFactor: 2, ...(phone ? {isMobile: true, hasTouch: true} : {})});
  const page = await ctx.newPage();
  page.on("pageerror", e => console.log("pageerror", e.message));
  await page.goto(base + "/generated/studio.html");
  await page.waitForFunction(() => window.KernStudio && window.KernStudio.frames() > 5, null, {timeout: 180000});
  await wait(1500);
  return {ctx, page, cmd: c => page.evaluate(c => window.KernStudio.command(c), c)};
}
const playing = page => page.waitForFunction(() => window.KernStudio.state.play.state === "playing", null, {timeout: 600000});

if (want("studio-play.png")) {
  const {ctx, page} = await studio();
  await page.click("#playBtn");
  await playing(page);
  await wait(6000);
  await shot(page, "studio-play.png");
  await ctx.close();
}
// The duck pictures stay as they are: Kern Studio now draws the duck with Pollen Robotics' meshes
// (CC BY-NC-SA 4.0, non-commercial), and a book that is sold must not show them. The committed
// duck-*.png show the author's own design; they are only remade with KERN_BOOK_DUCK=own-design,
// on a build whose duck is not Pollen's.
const duckAllowed = process.env.KERN_BOOK_DUCK === "own-design";
if (!duckAllowed && (only.length === 0 || only.some(n => n.startsWith("duck-"))))
  console.log("duck-*.png kept: the Studio's duck is Pollen's (CC BY-NC-SA), not for a book that is sold");
if (duckAllowed && (want("duck-level.png") || want("duck-play.png") || want("duck-close.png"))) {
  const {ctx, page, cmd} = await studio();
  await cmd({cmd: "new", template: "microDuck"});
  await wait(8000);
  if (want("duck-level.png")) await shot(page, "duck-level.png");
  if (want("duck-close.png")) {  // the duck up close (the cover), without the viewport's buttons and status line
    const level = JSON.parse(await page.evaluate(() => window.KernStudio.command({cmd: "packageText"}).text));
    const p = level.actors.find(a => a.name && a.name.startsWith("Micro Duck")).transform.position;
    const [ox, oy, oz] = [1.25, 0.5, 0.62];
    await cmd({cmd: "viewCamera", position: [p[0] + ox, p[1] + oy, p[2] + oz],
               yaw: Math.atan2(-oy, -ox) * 180 / Math.PI, pitch: -Math.atan2(oz - 0.3, Math.hypot(ox, oy)) * 180 / Math.PI});
    await cmd({cmd: "select", ids: []});
    await wait(4000);
    const r = await page.evaluate(() => window.KernStudio.viewportRect(0));
    await page.screenshot({path: path.join(out, "duck-close.png"), clip: {x: r.x, y: r.y + 40, width: r.width, height: r.height - 76}, timeout: 600000});
    console.log("duck-close.png");
    await cmd({cmd: "new", template: "microDuck"});
    await wait(6000);
  }
  if (want("duck-play.png")) {  // simulated: the duck is autonomous and plays on its own
    await cmd({cmd: "play", mode: "simulate"});
    await page.waitForFunction(() => window.KernStudio.state.play.state === "simulating", null, {timeout: 600000});
    await wait(45000);  // the duck walks up to the ball
    await shot(page, "duck-play.png");
  }
  await ctx.close();
}
if (want("studio-multiplayer.png")) {
  const {ctx, page, cmd} = await studio();
  await cmd({cmd: "netPlayers", players: 2});
  await cmd({cmd: "play", mode: "play"});
  await page.waitForFunction(() => window.KernStudio.status.net?.clients?.[0]?.hud?.status === "playing", null, {timeout: 120000}).catch(() => {});
  await wait(3000);
  await shot(page, "studio-multiplayer.png");
  await ctx.close();
}
if (want("studio-narrow.png")) {
  const {ctx, page} = await studio({width: 820, height: 900});
  await shot(page, "studio-narrow.png");
  await ctx.close();
}
if (want("phone-dock-390.png") || want("phone-play-390.png")) {
  const {ctx, page} = await studio({width: 390, height: 844}, true);
  if (want("phone-dock-390.png")) await shot(page, "phone-dock-390.png");
  if (want("phone-play-390.png")) {
    await page.tap("#playBtn");
    await playing(page);
    await wait(5000);
    await shot(page, "phone-play-390.png");
  }
  await ctx.close();
}
for (const [name, viewport, phone] of [["game.png", {width: 1280, height: 800}, false], ["game-phone.png", {width: 844, height: 390}, true]]) {
  if (!want(name) || !process.env.KERN_GAME_HTML) continue;  // a packaged game (Studio: File > Export), e.g. the e2e test's orb-run-game.html
  const ctx = await browser.newContext({viewport, deviceScaleFactor: 2, ...(phone ? {isMobile: true, hasTouch: true} : {})});
  const page = await ctx.newPage();
  await page.goto(process.env.KERN_GAME_HTML);
  await wait(15000);
  await shot(page, name);
  await ctx.close();
}
if (want("lehrer.png") && process.env.KERN_LEHRER_URL) {  // the Lehrer page as the site lays it out (lehrer/index.html)
  const ctx = await browser.newContext({viewport: {width: 1280, height: 800}, deviceScaleFactor: 2});
  const page = await ctx.newPage();
  await page.goto(process.env.KERN_LEHRER_URL);
  await wait(10000);
  await shot(page, "lehrer.png");
  await ctx.close();
}
await browser.close();
