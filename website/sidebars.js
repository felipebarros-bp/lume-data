// @ts-check
/** @type {import('@docusaurus/plugin-content-docs').SidebarsConfig} */
const sidebars = {
  docs: [
    'intro',
    {type: 'category', label: 'Getting started', collapsed: false, items: ['getting-started/install', 'getting-started/first-connection', 'getting-started/tour']},
    {type: 'category', label: 'Working with SQL', collapsed: false, items: ['sql/editor', 'sql/execution-plans']},
    {type: 'category', label: 'Exploring a database', collapsed: false, items: ['explore/schema-explorer', 'explore/er-diagrams', 'explore/table-designer']},
    'privacy',
  ],
};
export default sidebars;
