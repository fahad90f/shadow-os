import { useMemo, useState } from "react";
import { REPO_URL, formatSize, type RepoPackage } from "../manifest";

interface PackageTableProps {
  packages: RepoPackage[];
}

export function PackageTable({ packages }: PackageTableProps) {
  const [query, setQuery] = useState("");

  const shown = useMemo(() => {
    const needle = query.trim().toLowerCase();
    if (!needle) return packages;
    return packages.filter((item) => `${item.name} ${item.version} ${item.summary}`.toLowerCase().includes(needle));
  }, [packages, query]);

  return (
    <>
      <div className="filter">
        <span aria-live="polite">
          {shown.length} {shown.length === 1 ? "package" : "packages"}
        </span>
        <input
          type="search"
          value={query}
          onChange={(event) => setQuery(event.target.value)}
          placeholder="Filter packages"
          aria-label="Filter packages"
        />
      </div>
      <div className="table">
        <table>
          <thead>
            <tr>
              <th>Package</th>
              <th>Version</th>
              <th>What it is</th>
              <th className="size">Download</th>
            </tr>
          </thead>
          <tbody>
            {shown.map((item) => (
              <tr key={`${item.name}/${item.architecture}`}>
                <td className="name">{item.name}</td>
                <td className="version">{item.version}</td>
                <td className="summary">{item.summary}</td>
                <td className="size">
                  <a href={`${REPO_URL}/${item.filename}`}>{formatSize(item.size)}</a>
                </td>
              </tr>
            ))}
            {shown.length === 0 && (
              <tr>
                <td className="summary" colSpan={4}>
                  No package matches "{query.trim()}".
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </>
  );
}
