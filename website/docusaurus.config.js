// @ts-check
// Lume Data documentation (Docusaurus). Published to GitHub Pages by .github/workflows/docs.yml;
// each release freezes a version of these docs (docs:version, see the release workflow).
import {themes as prismThemes} from 'prism-react-renderer';
import fs from 'node:fs';

// released versions (newest first), written by `docusaurus docs:version` in the release workflow
const released = fs.existsSync('./versions.json') ? JSON.parse(fs.readFileSync('./versions.json', 'utf8')) : [];

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'Lume Data',
  tagline: 'A fast, lightweight, native database IDE.',
  favicon: 'img/favicon.ico',
  future: {v4: true},
  url: 'https://tessarotto21.github.io',
  baseUrl: '/lume-data/',
  organizationName: 'tessarotto21',
  projectName: 'lume-data',
  trailingSlash: false,
  onBrokenLinks: 'throw',
  markdown: {hooks: {onBrokenMarkdownLinks: 'throw'}},
  i18n: {defaultLocale: 'en', locales: ['en']},
  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          routeBasePath: 'docs',
          sidebarPath: './sidebars.js',
          editUrl: 'https://github.com/tessarotto21/lume-data/tree/main/website/',
          showLastUpdateTime: false,
          // the latest release is the default; "Next" (main) documents what is not released yet
          lastVersion: released.length ? released[0] : 'current',
          versions: released.length
            ? {current: {label: 'Next (unreleased)', path: 'next', banner: 'unreleased'}}
            : {current: {label: 'Next', banner: 'none'}},
        },
        blog: false,
        theme: {customCss: './src/css/custom.css'},
      }),
    ],
  ],
  themes: [
    [
      '@easyops-cn/docusaurus-search-local',
      /** local search: no external service, nothing about the reader leaves the page */
      ({hashed: true, docsRouteBasePath: 'docs', indexBlog: false, highlightSearchTermsOnTargetPage: true}),
    ],
  ],
  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      image: 'img/social-card.png',
      colorMode: {defaultMode: 'dark', respectPrefersColorScheme: true},
      navbar: {
        title: 'Lume Data',
        logo: {alt: 'Lume Data', src: 'img/logo.svg'},
        items: [
          {type: 'docSidebar', sidebarId: 'docs', position: 'left', label: 'Docs'},
          {type: 'docsVersionDropdown', position: 'right'},
          {href: 'https://github.com/tessarotto21/lume-data/releases/latest', label: 'Download', position: 'right'},
          {href: 'https://github.com/tessarotto21/lume-data', label: 'GitHub', position: 'right'},
        ],
      },
      footer: {
        style: 'dark',
        links: [
          {title: 'Docs', items: [{label: 'Getting started', to: '/docs/getting-started/install'}, {label: 'ER diagrams', to: '/docs/explore/er-diagrams'}, {label: 'Privacy', to: '/docs/privacy'}]},
          {title: 'Project', items: [{label: 'Releases', href: 'https://github.com/tessarotto21/lume-data/releases'}, {label: 'Report an issue', href: 'https://github.com/tessarotto21/lume-data/issues/new/choose'}, {label: 'Discussions', href: 'https://github.com/tessarotto21/lume-data/discussions'}]},
        ],
        copyright: `Copyright © 2026 Isabella Tessarotto. Lume Data is free software under the GNU AGPL-3.0.`,
      },
      prism: {theme: prismThemes.github, darkTheme: prismThemes.dracula, additionalLanguages: ['sql', 'bash']},
    }),
};

export default config;
