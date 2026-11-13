import { useEffect, useState } from "react";

interface TerminalProps {
  /** One shell command per entry. */
  commands: string[];
}

/** A block of shell commands with a button that copies them as one line. */
export function Terminal({ commands }: TerminalProps) {
  const [label, setLabel] = useState("Copy");

  useEffect(() => {
    if (label === "Copy") return;
    const timer = setTimeout(() => setLabel("Copy"), 1600);
    return () => clearTimeout(timer);
  }, [label]);

  async function copy() {
    try {
      await navigator.clipboard.writeText(commands.join(" && "));
      setLabel("Copied");
    } catch {
      setLabel("Select and copy");
    }
  }

  return (
    <div className="term">
      <div className="term-bar">
        <span>terminal</span>
        <button type="button" className="copy" onClick={copy}>
          {label}
        </button>
      </div>
      <pre>
        {commands.map((command) => (
          <span className="line" key={command}>
            <span className="prompt">$ </span>
            {command}
            {"\n"}
          </span>
        ))}
      </pre>
    </div>
  );
}
