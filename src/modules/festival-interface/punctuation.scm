;;; Punctuation modes

;; Copyright (C) 2003 Brailcom, o.p.s.

;; Author: Milan Zamazal <pdm@brailcom.org>

;; COPYRIGHT NOTICE

;; This program is free software; you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation; either version 2 of the License, or
;; (at your option) any later version.

;; This program is distributed in the hope that it will be useful, but
;; WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
;; or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
;; for more details.

;; You should have received a copy of the GNU General Public License
;; along with this program; if not, write to the Free Software
;; Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.


(require 'ttw)


(defvar punctuation-mode 'default)

(defvar punctuation-orig-token_to_words nil)
(defvar punctuation-orig-word_method)

(defvar punctuation-chars "[- !\"#$%&'()*+,./\\^_`:;<=>?@{|}~]")
(defvar punctuation-chars-2 "[][]")

;; Default English voice doesn't have defined pronunciation of punctuation
;; characters
(defvar punctuation-pronunciation
  '(("." "dot")
    ("," "comma")
    (";" "semicolon")
    (":" "colon")
    ("!" "exclamation" "mark")
    ("?" "question" "mark")
    ("-" "dash")
    ("'" "right" "quote")
    ("`" "left" "quote")
    ("\"" "double" "quote")
    ("(" "left" "parentheses")
    (")" "right" "parentheses")
    ("{" "left" "brace")
    ("(" "right" "brace")))

(define (punctuation-split-token token name ttw)
  (cond
   ((and (not (string-matches name
                              (string-append ".*" punctuation-chars ".*")))
         (not (string-matches name
                              (string-append ".*" punctuation-chars-2 ".*"))))
    (ttw token name))
   ((let ((char (substring name 0 1)))
      (or (string-matches char punctuation-chars)
          (string-matches char punctuation-chars-2)))
    (append (if (eq? punctuation-mode 'all)
                (let ((char (substring name 0 1)))
                  (if (and (member current-voice '(kal_diphone ked_diphone))
                           (assoc char punctuation-pronunciation))
                      (cdr (assoc char punctuation-pronunciation))
                      (ttw token char))))
            (punctuation-split-token token
                                     (substring name 1 (- (length name) 1))
                                     ttw)))
   (t
    (let ((i 1))
      (while (let ((char (substring name i 1)))
               (and (not (string-matches char punctuation-chars))
                    (not (string-matches char punctuation-chars-2))))
        (set! i (+ i 1)))
      (append (ttw token (substring name 0 i))
              (punctuation-split-token
               token (substring name i (- (length name) i)) ttw))))))
             
(define (punctuation-token-to-words token name ttw)
  (if (eq? punctuation-mode 'default)
      (ttw token name)
      (punctuation-split-token token name ttw)))

(define (punctuation-process-english-words utt)
  (if (and (eq? punctuation-mode 'all)
           (member (Parameter.get 'Language)
                   '(english britishenglish americanenglish)))
      (mapcar
       (lambda (w)
         (let ((trans (assoc (item.name w) punctuation-pronunciation)))
           (if trans
               (begin
                 (item.set_name w (car (cdr trans)))
                 (set! trans (cdr (cdr trans)))
                 (while trans
                   (item.insert w (list (car trans)))
                   (set! trans (cdr trans)))))))
       (utt.relation.items utt 'Word)))
  utt)

(define (punctuation-process-final-punctuation utt)
  (if (eq? punctuation-mode 'all)
      (mapcar
       (lambda (w)
         (let ((token (item.root (item.relation w 'Token))))
           (if (and (equal? (item.feat token 'punc) "0")
                    (or (not (item.next w))
                        (not (equal? token
                                     (item.root
                                      (item.relation (item.next w) 'Token))))))
               (begin
                 (item.insert w '(what-should-be-here? ((name "."))))
                 (item.append_daughter token (item.next w))))))
       (utt.relation.items utt 'Word)))
  utt)

(define (punctuation-word_method utt)
  (if (eq? punctuation-mode 'all)
      (mapcar
       (lambda (w)
         (let ((pos (item.feat w "pos")))
           (if (or (string-equal "punc" pos)
                   (string-equal "fpunc" pos))
               (item.set_feat w "pos" "allpunc"))))
       (utt.relation.items utt 'Word)))
  (punctuation-orig-word_method utt))

(define (setup-punctuation-mode)
  (ttw-setup)
  (add-hook ttw-token-to-words-funcs punctuation-token-to-words)
  (add-hook ttw-token-method-hook punctuation-process-english-words)
  (add-hook ttw-token-method-hook punctuation-process-final-punctuation)
  (if (not (eq? (Param.get 'Word_Method) punctuation-word_method))
      (begin
        (set! punctuation-orig-word_method (Param.get 'Word_Method))
        (Param.set 'Word_Method punctuation-word_method))))

(define (set-punctuation-mode mode)
  (if (member mode '(default all none))
      (setup-punctuation-mode)
      (error "Unknown punctuation mode" mode))
  (set! punctuation-mode mode))


(provide 'punctuation)