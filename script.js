(() => {
  const discordHandle = 'ZelRaccy';

  // Site data (data/site-data.js, written by the manager app) is requested with a new ?t= on every visit. A new URL
  // can't be answered from the browser's or GitHub's cache, so the page always shows the latest published data. If
  // that fails, the plain URL is tried once; after that the page carries on with no data. Anything missing or
  // malformed is treated as empty. The status pill, slideshow and gallery are filled in once the data has arrived.
  function loadSiteData(done) {
    const attempt = (src, next) => {
      const script = document.createElement('script');
      script.src = src;
      script.onload = () => done(window.SITE_DATA);
      script.onerror = () => {
        script.remove();
        next();
      };
      document.head.appendChild(script);
    };
    attempt(`data/site-data.js?t=${Date.now()}`, () => attempt('data/site-data.js', () => done(null)));
  }

  loadSiteData(data => {
    const siteData = data && typeof data === 'object' ? data : {};
    const galleryItems = galleryEntries(siteData);
    renderStatus(siteData.commissionsOpen);
    renderSlideshow(galleryItems);
    renderGallery(galleryItems);
  });

  const year = document.getElementById('year');
  if (year) year.textContent = new Date().getFullYear();

  // Mobile menu
  const menuToggle = document.querySelector('.menu-toggle');
  const navLinks = document.getElementById('nav-links');
  if (menuToggle && navLinks) {
    const setMenu = open => {
      navLinks.classList.toggle('open', open);
      menuToggle.setAttribute('aria-expanded', String(open));
      menuToggle.setAttribute('aria-label', open ? 'Close navigation' : 'Open navigation');
    };
    menuToggle.addEventListener('click', () => setMenu(!navLinks.classList.contains('open')));
    navLinks.querySelectorAll('a').forEach(link => link.addEventListener('click', () => setMenu(false)));
    document.addEventListener('keydown', event => {
      if (event.key === 'Escape' && navLinks.classList.contains('open')) {
        setMenu(false);
        menuToggle.focus();
      }
    });
  }

  // Commission status pill (stays hidden until the site data has arrived)
  function renderStatus(open) {
    const status = document.getElementById('commission-status');
    if (!status || typeof open !== 'boolean') return;
    status.classList.add(open ? 'is-open' : 'is-closed');
    status.querySelector('.status-text').textContent = open ? 'Commissions: Open' : 'Commissions: Closed';
    status.hidden = false;
  }

  // Use the hosted profile picture when it loads, otherwise fall back to Ember.
  const contactAvatar = document.getElementById('contact-avatar');
  if (contactAvatar) {
    contactAvatar.addEventListener('error', () => {
      if (contactAvatar.dataset.fallbackApplied) return;
      contactAvatar.dataset.fallbackApplied = 'true';
      contactAvatar.src = 'assets/gallery/ember.webp';
      contactAvatar.alt = 'Ember';
    });
  }

  // Gallery entries from the site data. Images in the site folder get ?v=<version> ("version" in site-data.js, which
  // the manager app changes on every save), so a replaced image is downloaded again instead of coming from a cache.
  // If an image fails from its relative path, it is retried once straight from the GitHub repository (same version).
  const repoFiles = 'https://raw.githubusercontent.com/Xavokai/CommissionSIte/main/';
  function galleryEntries(siteData) {
    const version = typeof siteData.version === 'string' ? siteData.version.trim() : '';
    const withVersion = url => (version ? `${url}${url.includes('?') ? '&' : '?'}v=${encodeURIComponent(version)}` : url);
    return (Array.isArray(siteData.gallery) ? siteData.gallery : [])
      .filter(item => item && typeof item.file === 'string' && item.file.trim())
      .map(item => {
        const file = item.file.trim();
        const external = /^([a-z][a-z0-9+.-]*:|\/\/)/i.test(file);
        return {
          name: typeof item.name === 'string' ? item.name.trim() : '',
          src: external ? file : withVersion(file),
          fallback: external ? '' : withVersion(repoFiles + file.replace(/^\.?\/+/, '')),
        };
      });
  }

  function useRepoFallback(img, item, link) {
    img.addEventListener('error', () => {
      if (img.dataset.fallbackApplied || !item.fallback) return;
      img.dataset.fallbackApplied = 'true';
      img.src = item.fallback;
      if (link) link.href = img.src;
    });
  }

  // Hero slideshow
  function renderSlideshow(galleryItems) {
    const showcase = document.getElementById('showcase');
    const slideshow = document.getElementById('slideshow');
    if (!showcase || !slideshow) return;
    if (!galleryItems.length) {
      showcase.hidden = true;
      showcase.closest('.hero').classList.add('hero-solo');
    } else {
      const frame = slideshow.querySelector('.slideshow-frame');
      const controls = slideshow.querySelector('.slideshow-controls');
      const dotsWrap = slideshow.querySelector('.slideshow-dots');
      const nameLabel = showcase.querySelector('.slideshow-name');
      const slides = [];
      const dots = [];
      let current = 0;
      let timer = null;
      let hovered = false;
      let focused = false;

      galleryItems.forEach((item, index) => {
        const img = document.createElement('img');
        img.className = 'slide';
        useRepoFallback(img, item);
        img.src = item.src;
        img.alt = item.name;
        img.width = 1920;
        img.height = 1080;
        img.decoding = 'async';
        frame.appendChild(img);
        slides.push(img);

        const dot = document.createElement('button');
        dot.type = 'button';
        dot.className = 'slideshow-dot';
        dot.setAttribute('aria-label', item.name || `Image ${index + 1}`);
        dot.addEventListener('click', () => show(index, true));
        dotsWrap.appendChild(dot);
        dots.push(dot);
      });

      function show(index, manual) {
        current = (index + slides.length) % slides.length;
        slides.forEach((slide, i) => {
          const active = i === current;
          slide.classList.toggle('is-active', active);
          if (active) slide.removeAttribute('aria-hidden');
          else slide.setAttribute('aria-hidden', 'true');
        });
        dots.forEach((dot, i) => {
          if (i === current) dot.setAttribute('aria-current', 'true');
          else dot.removeAttribute('aria-current');
        });
        if (nameLabel) nameLabel.textContent = galleryItems[current].name;
        if (manual) restart();
      }

      function stop() {
        window.clearInterval(timer);
        timer = null;
      }

      function restart() {
        stop();
        if (slides.length > 1 && !hovered && !focused && !document.hidden) {
          timer = window.setInterval(() => show(current + 1, false), 5000);
        }
      }

      if (slides.length > 1) {
        controls.hidden = false;
        controls.querySelectorAll('[data-step]').forEach(button => {
          button.addEventListener('click', () => show(current + Number(button.dataset.step), true));
        });
        slideshow.addEventListener('mouseenter', () => { hovered = true; stop(); });
        slideshow.addEventListener('mouseleave', () => { hovered = false; restart(); });
        slideshow.addEventListener('focusin', () => { focused = true; stop(); });
        slideshow.addEventListener('focusout', event => {
          if (slideshow.contains(event.relatedTarget)) return;
          focused = false;
          restart();
        });
        document.addEventListener('visibilitychange', restart);
      }

      show(0, false);
      restart();
    }
  }

  // Column spans (out of 12) chosen so no row of the gallery is left part-empty, whatever the item count.
  // Wide screens: the first item is a two-row featured tile with two or four tiles beside it, and the rest fill
  // rows of two to four. Counts with no such arrangement (1, 2, 4) skip the featured tile and use even rows.
  // Tablet: rows of two, with the first item full width when the count is odd.
  function gallerySpans(count) {
    const evenRows = n => {
      const rows = Math.ceil(n / 4);
      return Array.from({ length: rows }, (_, i) => Math.floor(n / rows) + (i < n % rows ? 1 : 0))
        .flatMap(length => Array(length).fill(12 / length));
    };
    const beside = count === 6 || count < 5 ? 2 : 4;
    const featured = count >= 3 && count - 1 - beside !== 1;
    return {
      featured,
      wide: featured ? [...(beside === 4 ? [6, 3, 3, 3, 3] : [8, 4, 4]), ...evenRows(count - 1 - beside)] : evenRows(count),
      tablet: Array.from({ length: count }, (_, i) => (i === 0 && count % 2 ? 12 : 6)),
    };
  }

  // Gallery, rendered from site data.
  const galleryGrid = document.getElementById('gallery-grid');
  function renderGallery(galleryItems) {
    if (!galleryGrid) return;
    const spans = gallerySpans(galleryItems.length);
    galleryItems.forEach((item, index) => {
      const figure = document.createElement('figure');
      figure.className = index === 0 && spans.featured ? 'gallery-item gallery-item-featured' : 'gallery-item';
      figure.style.setProperty('--span', spans.wide[index]);
      figure.style.setProperty('--span-md', spans.tablet[index]);

      const link = document.createElement('a');
      link.className = 'gallery-link';
      link.href = item.src;
      link.target = '_blank';
      link.rel = 'noopener';

      const img = document.createElement('img');
      useRepoFallback(img, item, link);
      img.src = item.src;
      img.alt = item.name;
      img.width = 1920;
      img.height = 1080;
      img.loading = 'lazy';
      img.decoding = 'async';
      link.appendChild(img);

      const caption = document.createElement('figcaption');
      const title = document.createElement('span');
      title.className = 'gallery-title';
      title.textContent = item.name;
      caption.appendChild(title);

      figure.append(link, caption);
      galleryGrid.appendChild(figure);
    });
  }

  // Gallery lightbox. Without <dialog> support the links just open the image in a new tab.
  const lightbox = document.getElementById('lightbox');
  if (lightbox && galleryGrid && typeof lightbox.showModal === 'function') {
    const fullImage = lightbox.querySelector('.lightbox-image');
    const caption = lightbox.querySelector('.lightbox-caption');
    let opener = null;

    galleryGrid.addEventListener('click', event => {
      const link = event.target.closest('.gallery-link');
      if (!link || event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
      event.preventDefault();
      const figure = link.closest('figure');
      const title = figure && figure.querySelector('.gallery-title');
      const thumb = link.querySelector('img');
      fullImage.src = link.href;
      fullImage.alt = thumb ? thumb.alt : '';
      caption.textContent = title ? title.textContent.trim() : '';
      opener = link;
      lightbox.showModal();
    });

    // Clicking anywhere outside the image closes it; Escape is handled by <dialog>.
    lightbox.addEventListener('click', event => {
      if (event.target !== fullImage) lightbox.close();
    });
    lightbox.addEventListener('close', () => {
      fullImage.removeAttribute('src');
      if (opener) opener.focus();
    });
  }

  // Toast
  const toast = document.getElementById('toast');
  let toastTimer;
  function showToast(message) {
    if (!toast) return;
    toast.textContent = message;
    toast.classList.add('is-visible');
    window.clearTimeout(toastTimer);
    toastTimer = window.setTimeout(() => toast.classList.remove('is-visible'), 2000);
  }

  // Copy Discord handle
  function copyWithExecCommand(text) {
    const textarea = document.createElement('textarea');
    textarea.value = text;
    textarea.setAttribute('readonly', '');
    textarea.style.position = 'fixed';
    textarea.style.top = '0';
    textarea.style.opacity = '0';
    document.body.appendChild(textarea);
    textarea.select();
    let success = false;
    try {
      success = document.execCommand('copy');
    } catch (_) {
      success = false;
    }
    textarea.remove();
    return success;
  }

  async function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) {
      try {
        await navigator.clipboard.writeText(text);
        return true;
      } catch (_) {
        // Fall through to the legacy method.
      }
    }
    return copyWithExecCommand(text);
  }

  const copyButton = document.getElementById('copy-discord');
  if (copyButton) {
    copyButton.addEventListener('click', async () => {
      const copied = await copyText(discordHandle);
      showToast(copied ? 'Copied!' : "Couldn't copy automatically. Search for ZelRaccy on Discord.");
    });
  }

  // Background music: song.mp3 next to index.html, looping, muted until the visitor turns it on.
  const music = document.getElementById('music');
  const audio = document.getElementById('bg-music');
  if (music && audio) {
    const toggle = document.getElementById('music-toggle');
    const volume = document.getElementById('music-volume');

    const sync = () => {
      const on = !audio.muted && !audio.paused;
      toggle.setAttribute('aria-pressed', String(on));
      toggle.title = on ? 'Mute music' : 'Play music';
      music.classList.toggle('is-on', on);
    };

    const tryPlay = () => audio.play().catch(() => {}).then(sync);

    const hide = () => {
      music.hidden = true;
      audio.removeAttribute('src');
    };

    if (audio.error) {
      hide();
    } else {
      audio.addEventListener('error', hide);
      audio.volume = Number(volume.value);
      audio.muted = true;
      music.hidden = false;
      tryPlay();

      toggle.addEventListener('click', () => {
        if (audio.muted || audio.paused) {
          audio.muted = false;
          if (audio.volume === 0) {
            audio.volume = 0.5;
            volume.value = '0.5';
          }
          tryPlay();
        } else {
          audio.muted = true;
          sync();
        }
      });

      volume.addEventListener('input', () => {
        audio.volume = Number(volume.value);
        if (audio.volume > 0 && audio.muted) {
          audio.muted = false;
          tryPlay();
        } else if (audio.volume === 0 && !audio.muted) {
          audio.muted = true;
          sync();
        }
      });

      audio.addEventListener('play', sync);
      audio.addEventListener('pause', sync);
      audio.addEventListener('volumechange', sync);
    }
  }

  // sw.js makes returning visitors get the page, styles, scripts and site data straight from GitHub instead of from a
  // cache (see README.md). Service workers only run on http(s), so opening index.html from disk skips this.
  if ('serviceWorker' in navigator && /^https?:$/.test(window.location.protocol)) {
    window.addEventListener('load', () => {
      navigator.serviceWorker.register('sw.js').catch(() => {});
    });
  }
})();
