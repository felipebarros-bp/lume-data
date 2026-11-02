import Link from '@docusaurus/Link';
import useBaseUrl from '@docusaurus/useBaseUrl';
import Layout from '@theme/Layout';

const features = [
  {title: 'Fast and light', text: 'Native C and SDL3: starts in a blink, stays idle at 0 % CPU, and keeps large result sets out of memory it does not need.'},
  {title: 'Big databases', text: 'Thousands of schemas and tens of thousands of tables, read in a few bulk queries, searchable, never all drawn at once.'},
  {title: 'Understand the structure', text: 'ER diagrams from real foreign keys, execution plans, object and table editors that review every change before it runs.'},
  {title: 'Private by default', text: 'Passwords in the OS secret store. Usage statistics off unless you turn them on. The AI sees nothing without your consent.'},
];

export default function Home() {
  return (
    <Layout title="Lume Data" description="A fast, lightweight, native database IDE">
      <header className="hero hero--lume">
        <div className="container">
          <img className="hero-logo" src={useBaseUrl('/img/logo.svg')} alt="" />
          <h1 className="hero__title">Lume Data</h1>
          <p className="hero__subtitle">A fast, lightweight, native database IDE for SQLite, PostgreSQL, MySQL / MariaDB, SQL Server and Oracle.</p>
          <div className="buttons">
            <Link className="button button--primary button--lg" to="/docs/getting-started/install">Get started</Link>
            <Link className="button button--secondary button--lg" href="https://github.com/tessarotto21/lume-data/releases/latest">Download</Link>
          </div>
        </div>
      </header>
      <main className="container">
        <img className="shot" src={useBaseUrl('/img/screens/query-results.png')} alt="The SQL editor with results" />
        <div className="features">
          {features.map((f) => (<div key={f.title}><h3>{f.title}</h3><p>{f.text}</p></div>))}
        </div>
      </main>
    </Layout>
  );
}
