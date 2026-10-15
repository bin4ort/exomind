(define-module (exomind)
  #:use-module (guix licenses)
  #:use-module (guix packages)
  #:use-module (guix build-system gnu)
  #:use-module (guix download)
  #:use-module (guix git-download)
  #:use-module (guix utils))

(define-public exomind
  (package
    (name "exomind")
    (version "0.4.1alpha")
    (source
     (origin
       (method git-fetch)
       (uri (git-reference
             (url "https://github.com/bin4ort/exomind")
             (commit (string-append "v" version))))
       (file-name (git-file-name name version))
       (sha256
        (base32 "0000000000000000000000000000000000000000000000000000")))) ; TODO: fill in

    (build-system gnu-build-system)
    (arguments
     '(#:make-flags '("qms-modules")
       #:phases
       (modify-phases %standard-phases
         (add-after 'unpack 'set-source-date
           (lambda* (#:key source-date-epoch #:allow-other-keys)
             (setenv "SOURCE_DATE_EPOCH" (number->string source-date-epoch))
             #t)))))

    (native-inputs
     `(("gcc" ,gcc)
       ("make" ,make)
       ("bash" ,bash)
       ("coreutils" ,coreutils)))

    (inputs
     `(("curl" ,curl)))

    (home-page "https://github.com/bin4ort/exomind")
    (synopsis "AINSS - AI-Native Software Stack for AI agents")
    (description "Exomind is an AI-native software stack providing durable memory, scheduling, orchestration, documentation audit, quality management, research, context continuity, and a behavioral development kit for AI agents.")
    (license gpl3+)))