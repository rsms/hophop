function generateMarkdownPage(site, page, write_file) {
	if (page.srctype != 'md' || !page.outfile.endsWith('.html'))
		return

	let s = site.renderPage({...site, }, page)

	s = s.replace(/\]\(([^\)]+)\.html\)(?!\b|$)/g, (all, href) => {
		if (href[0] == '/') {
			if (!href.startsWith(site.baseURL))
				return `](${href}.html)`
			href = path.relative(page.url, href)
		}
		return `](${href}.md)`
	})

	s = s.replace(/\n\n<\!--[^>]*-->\n\n/mg, "\n\n")
	s = s.replace(/<\!--[^>]*-->\n?/mg, "")

	s = s.trim() + '\n'

	const outfile = page.outfile.substring(0, page.outfile.length - 4) + "md"
	return write_file(outfile, s)
}

// this function is called by mkweb when it starts
module.exports = ({
	site,     // mutable object describing the site
	hljs,     // HighlightJS module (NPM: highlight.js)
	markdown, // Markdown module (NPM: markdown-wasm)
	glob,     // glob function (NPM: miniglob)
	cli_opts,
	mtime,
	build_site,
	read_file,  // "node:fs/promises".readFile
	write_file, // "node:fs/promises".writeFile + mkdir if needed
}) => {
	site.social_image = "card.png"
	site.outdir = cli_opts.outdir == "_site" ? "build/_site" : cli_opts.outdir

	// console.log('mkweb site =', site)

	// ignore some source files
	site.ignoreFilter = (defaultIgnoreFilter => (name, path) => {
		if (path == "build" || path == "serve.sh")
			return true
		let ignore = defaultIgnoreFilter.call(site, name, path)
		if (ignore && path.indexOf('.ds_store') == -1)
			console.log("exclude", path)
		return ignore
	})(site.ignoreFilter)

	// workaround for a bug (bad behavior) in markdown-wasm
	const md1 = site.pageRenderers.md
	site.pageRenderers.md = (site, page) => {
		let html = md1(site, page)

		// Replace pure-anchor in header to interactive anchor.
		//
		// First, we replace
		//   <h2><code><a id="foo" class="anchor" aria-hidden="true" href="#foo"></a>Foo</code></h2>
		// with:
		//   <h2 id="foo"><code><a href="#foo">Foo</a></code></h2>
		//
		// Then, we replace
		//   <h2><a id="foo" class="anchor" aria-hidden="true" href="#foo"></a>Foo</h2>
		// with:
		//   <h2 id="foo"><a href="#foo">Foo</a></h2>
		//
		html = html.replace(
			/(<h[2-6])><code><a( id="[^"]+") class="anchor" aria-hidden="true"(.+)<\/a>(.+)<\/code>(<\/h[2-6]+>)/g,
			'$1$2><code><a$3$4</a></code>$5')
		html = html.replace(
			/(<h[2-6])><a( id="[^"]+") class="anchor" aria-hidden="true"(.+)<\/a>([^<]+)/g,
			'$1$2><a$3$4</a>')

		return html
	}

	site.onBeforePage = (page) => {
		// strip comments
		page.html = page.html.replace(/<!--[\s\S]*?-->/g, "")
	}

	// include .md pages
	site.onAfterPage = (page) => generateMarkdownPage(site, page, write_file)

	// don't rebuild when some file changes
	// site.ignoreWatchFilter = (name, path) =>
	//   path == "externally-generated.html"

	// these optional callbacks can return a Promise to cause build process to wait

	// site.onBeforeBuild = async (files) => {
	// 	// called when .pages has been populated
	// 	//console.log("[onBeforeBuild] pages:", site.pages)
	// 	//console.log("[onBeforeBuild] files:", files)
	// }

	// site.onAfterBuild = (files) => {
	// 	// called after site has been generated
	// 	// console.log("[onAfterBuild]", files)
	// 	fs.copyFileSync(`${site.srcdir}/_redirects`, `${site.outdir}/_redirects`)
	// }
}
