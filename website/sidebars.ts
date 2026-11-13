import type {SidebarsConfig} from '@docusaurus/plugin-content-docs';

const sidebars: SidebarsConfig = {
  docsSidebar: [
    'index',
    'getting-started',
    {
      type: 'category',
      label: 'Guides',
      items: ['examples', 'online', 'online-details', 'costs', 'differences', 'performance', 'talks'],
    },
    {
      type: 'category',
      label: 'Reference',
      items: ['reference/functions', 'reference/costs-and-penalties', 'reference/algorithms'],
    },
    {
      type: 'category',
      label: 'Development',
      link: {type: 'doc', id: 'development/index'},
      items: ['development/architecture', 'development/testing'],
    },
    'references',
  ],
};

export default sidebars;
