ok(!!caches, 'caches object should be available on global');
caches.create('snafu').then(function(createCache) {
  ok(!!createCache, 'cache object should be resolved from caches.create');
  caches.get('snafu').then(function(getCache) {
    ok(!!getCache, 'cache object should be resolved from caches.get');
    caches.has('snafu').then(function(hasResult) {
      ok(hasResult, 'caches.has() should resolve true');
      caches.keys().then(function(keys) {
        ok(!!keys, 'caches.keys() should resolve to a truthy value');
        is(1, keys.length, 'caches.keys() should resolve to an array of length 1');
        is(0, keys.indexOf('snafu'), 'caches.keys() should resolve to an array containing key');
        caches.delete('snafu').then(function(deleteResult) {
          ok(deleteResult, 'caches.delete() should resolve true');
          caches.get('snafu').then(function(getMissingCache) {
            is(undefined, getMissingCache, 'missing key should resolve to undefined cache');
            workerTestDone();
          });
        });
      });
    });
  });
});
