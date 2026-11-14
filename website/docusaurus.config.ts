import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// Docs for the hugr-lab DuckDB change point extension (cpt). Set up as hugr-lab/mssql-extension's docs are,
// so the hugr-lab sites share look and feel; published by this repo's Pages workflow (pages.yml) to
// https://hugr-lab.github.io/duck-changepoint/. The pages in docs/ are built from the repository's
// README.md, DEVELOPMENT.md and docs/*.md; keep them in step when either changes.

const config: Config = {
  title: 'DuckDB Changepoint Extension',
  tagline: 'Offline and online change point detection in SQL: PELT, binary segmentation, dynamic programming, CROPS.',
  favicon: 'img/favicon.ico',

  url: 'https://hugr-lab.github.io',
  baseUrl: '/duck-changepoint/',
  trailingSlash: true,

  organizationName: 'hugr-lab',
  projectName: 'duck-changepoint',

  // 'throw': docs-build.yml is the PR gate for website/, and a gate that exits 0 on a broken link or
  // anchor does not gate anything.
  onBrokenLinks: 'throw',
  onBrokenAnchors: 'throw',

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          sidebarPath: './sidebars.ts',
          // Docs ARE the site: /duck-changepoint/<page>/
          routeBasePath: '/',
          // VERSIONING CONTRACT, as in mssql-extension: at each release, run
          //   npm run docusaurus docs:version <X.Y.Z>
          // in website/ and commit the snapshot. Docusaurus then serves the latest released version at the
          // root and the live docs/ tree as "Next" under /next/ with an "unreleased" banner. Until the first
          // snapshot exists, the current docs serve at the root.
          // No editUrl: every page is generated, and sync_pages.py gives each one a custom_edit_url to the
          // file it is built from (README.md, DEVELOPMENT.md or docs/*.md).
          showLastUpdateTime: true,
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themes: ['@docusaurus/theme-mermaid'],

  markdown: {
    mermaid: true,
    // .md pages are CommonMark, not MDX: they are built from the repository's Markdown, where a < or { in
    // the text is text, not JSX.
    format: 'detect',
    hooks: {
      onBrokenMarkdownLinks: 'throw',
    },
  },

  themeConfig: {
    metadata: [
      {name: 'keywords', content: 'DuckDB, change point detection, PELT, binary segmentation, CROPS, time series, extension'},
      {name: 'description', content: 'DuckDB extension for offline and online change point detection: PELT, binary segmentation, dynamic programming and CROPS over four segment costs.'},
    ],
    navbar: {
      title: 'Changepoint Extension',
      logo: {
        alt: 'Hugr Lab',
        src: 'img/logo-circle.svg',
        href: '/',
      },
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'docsSidebar',
          position: 'left',
          label: 'Docs',
        },
        {
          type: 'docsVersionDropdown',
          position: 'right',
        },
        {
          href: 'https://hugr-lab.github.io/',
          label: 'Hugr Lab',
          position: 'right',
        },
        {
          href: 'https://github.com/hugr-lab/duck-changepoint',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    colorMode: {
      defaultMode: 'light',
      disableSwitch: true,
      respectPrefersColorScheme: false,
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Docs',
          items: [
            {label: 'Getting Started', to: '/getting-started/'},
            {label: 'Functions', to: '/reference/functions/'},
            {label: 'Examples', to: '/examples/'},
          ],
        },
        {
          title: 'Community',
          items: [
            {label: 'GitHub', href: 'https://github.com/hugr-lab/duck-changepoint'},
            {label: 'Issues', href: 'https://github.com/hugr-lab/duck-changepoint/issues'},
            {label: 'DuckDB Community Extensions', href: 'https://duckdb.org/community_extensions/extensions/cpt.html'},
          ],
        },
        {
          title: 'Hugr Lab',
          items: [{label: 'Main site', href: 'https://hugr-lab.github.io/'}],
        },
        {
          title: 'Legal',
          items: [
            {label: 'Impressum', href: 'https://hugr-lab.github.io/impressum/'},
            {label: 'Datenschutz', href: 'https://hugr-lab.github.io/datenschutz/'},
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} Hugr Lab.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['sql', 'bash', 'r', 'python'],
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
