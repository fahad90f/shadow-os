import { PackageTable } from "./components/PackageTable";
import { Terminal } from "./components/Terminal";
import { REPO_URL, formatFingerprint, formatPublished, useManifest, type RepoManifest } from "./manifest";

const UPDATE_COMMAND = "sudo apt update && sudo apt upgrade";

function Facts({ manifest }: { manifest: RepoManifest }) {
  const commit = manifest.commit.slice(0, 10);
  return (
    <dl className="facts">
      <div>
        <dt>Latest build</dt>
        <dd className="live">{manifest.build}</dd>
      </div>
      <div>
        <dt>Published (UTC)</dt>
        <dd>{formatPublished(manifest.published)}</dd>
      </div>
      <div>
        <dt>Source commit</dt>
        <dd>
          {manifest.commit && manifest.sourceUrl ? (
            <a href={`${manifest.sourceUrl}/commit/${manifest.commit}`}>{commit}</a>
          ) : (
            commit || "not recorded"
          )}
        </dd>
      </div>
      <div>
        <dt>Packages</dt>
        <dd>
          {manifest.packages.length} · {manifest.architectures.join(", ")}
        </dd>
      </div>
    </dl>
  );
}

export function App() {
  const state = useManifest();
  const manifest = state.status === "ready" ? state.manifest : null;
  const name = manifest?.name ?? "Shadow OS";

  return (
    <main>
      <header>
        <div className="brand">
          {name} <span>Updates</span>
        </div>
        {manifest && (
          <div className="channel">
            {manifest.version} "{manifest.codename}" · channel {manifest.channel}
          </div>
        )}
      </header>

      <h1>One command keeps {name} current.</h1>
      <p className="lede">
        Every change pushed to the source is built, tested and published here. A system installed from a current image
        picks it up with APT, together with Debian's own updates.
      </p>

      <Terminal commands={[UPDATE_COMMAND]} />

      {state.status === "loading" && <p className="notice">Looking up the latest build…</p>}
      {state.status === "unpublished" && (
        <p className="notice">
          No build has been published yet, or the repository at <a href={REPO_URL}>{REPO_URL}</a> cannot be reached
          right now. The command above only finds Shadow OS updates once a build is there.
        </p>
      )}
      {manifest && <Facts manifest={manifest} />}

      {manifest && (
        <section>
          <h2>Packages in this build</h2>
          <p>
            Installed packages older than these are upgraded. Debian packages come from Debian's servers, not from
            here.
          </p>
          <PackageTable packages={manifest.packages} />
        </section>
      )}

      {manifest && (
        <section>
          <h2>Installed from an older image?</h2>
          <p>
            An image built before the repository existed has its Shadow OS source switched off and trusts a different
            key. Switch it over once; after that the command above is all it needs.
          </p>
          <Terminal commands={[`curl -fsSLO ${REPO_URL}/enable-updates.sh`, "sudo sh enable-updates.sh"]} />
          <ol className="steps">
            <li>
              Installs the current <code>shadow-archive-keyring</code> package: the repository key and the APT source.
            </li>
            <li>Turns off the package snapshot that shipped inside the old image, because it is signed with the old key.</li>
            <li>
              Refreshes the package lists. Read <a href={`${REPO_URL}/enable-updates.sh`}>the script</a> first if you
              like; it is short.
            </li>
          </ol>
        </section>
      )}

      {manifest?.fingerprint && (
        <section>
          <h2>Signing key</h2>
          <p>
            APT accepts packages only when the index is signed by this key. It is installed as{" "}
            <code>/usr/share/keyrings/shadow-archive-keyring.gpg</code> (
            <a href={`${REPO_URL}/shadow-archive-keyring.gpg`}>download</a>).
          </p>
          <div className="fingerprint">{formatFingerprint(manifest.fingerprint)}</div>
        </section>
      )}

      <footer>
        Packages are served from <a href={REPO_URL}>{REPO_URL}</a>
        {manifest && (
          <>
            {" "}
            (<a href={`${REPO_URL}/dists/${manifest.codename}/InRelease`}>signed index</a>)
          </>
        )}
        .
        {manifest?.sourceUrl && (
          <>
            {" "}
            Source: <a href={manifest.sourceUrl}>{manifest.sourceUrl}</a>.
          </>
        )}
      </footer>
    </main>
  );
}
