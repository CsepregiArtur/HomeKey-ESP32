/*
 * 404 recovery for the HomeKey-ESP32 wiki.
 *
 * Broken internal links on this site have one characteristic shape: a page name
 * nested under another page path, because the link lost its leading slash and the
 * browser resolved it against the current directory. Examples:
 *
 *   /configuration/guest-tags   ->  should be  /guest-tags/
 *   /setup/updates              ->  should be  /updates/
 *   /path2_security_rollout/security -> should be /security/
 *
 * When the 404 page loads, try the trailing path segment (and the last two joined
 * with a dash) as a page at the site root. If one answers, go there instead of
 * leaving the reader stranded. If neither does, the page stays as the fallback.
 *
 * Kept as a static asset rather than an inline <script> because Hugo's HTML
 * minifier (used by the deploy workflow) strips inline scripts from the 404 layout.
 */
(function () {
  var url = new URL(window.location.href);
  var base = url.pathname.split('/').slice(0, 2).join('/') + '/';
  // base is /HomeKey-ESP32/ , so the site root is that prefix
  var root = url.origin + base;
  var rest = url.pathname.slice(base.length).split('/').filter(Boolean);
  if (rest.length < 2) return;

  var segment = rest[rest.length - 1];
  var joined = rest.slice(-2).join('-');
  var candidates = segment === joined ? [segment] : [segment, joined];

  function probe(i) {
    if (i >= candidates.length) return;
    var target = root + candidates[i] + '/';
    if (target === window.location.href) return;
    fetch(target, { method: 'HEAD' })
      .then(function (r) {
        if (r.ok) window.location.replace(target);
        else probe(i + 1);
      })
      .catch(function () { probe(i + 1); });
  }
  probe(0);
})();
