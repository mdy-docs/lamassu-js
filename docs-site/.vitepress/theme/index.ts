/*
 * Default VitePress theme plus the one component the docs add: the playground.
 * Registering it globally lets a markdown page drop <Playground /> in without
 * per-page imports.
 */
import DefaultTheme from "vitepress/theme";
import type { Theme } from "vitepress";
import Playground from "./Playground.vue";

export default {
  extends: DefaultTheme,
  enhanceApp({ app }) {
    app.component("Playground", Playground);
  },
} satisfies Theme;
