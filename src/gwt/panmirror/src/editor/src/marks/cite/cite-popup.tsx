/*
 * cite-popup.tsx
 *
 * Copyright (C) 2020 by RStudio, PBC
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */


import { Schema } from "prosemirror-model";
import { PluginKey } from "prosemirror-state";
import { DecorationSet, EditorView } from "prosemirror-view";

import React from "react";

import { EditorUI } from "../../api/ui";
import { textPopupDecorationPlugin, TextPopupTarget } from "../../api/text-popup";
import { WidgetProps } from "../../api/widgets/react";
import { Popup } from "../../api/widgets/popup";
import { LinkButton } from "../../api/widgets/button";
import { join } from "../../api/path";
import { BibliographySource, BibliographyManager, cslFromDoc } from "../../api/bibliography";
import { PandocServer } from "../../api/pandoc";

import './cite-popup.css';


const kMaxWidth = 400; // also in cite-popup.css

export function citePopupPlugin(schema: Schema, ui: EditorUI, bibMgr: BibliographyManager, server: PandocServer) {

  return textPopupDecorationPlugin({
    key: new PluginKey<DecorationSet>('cite-popup'),
    markType: schema.marks.cite_id,
    maxWidth: kMaxWidth,
    dismissOnEdit: true,
    createPopup: async (view: EditorView, target: TextPopupTarget, style: React.CSSProperties) => {
      await bibMgr.loadBibliography(ui, view.state.doc);

      const csl = cslFromDoc(view.state.doc);
      const citeId = target.text.replace(/^-@|^@/, '');
      const source = bibMgr.findCiteId(citeId);
      if (source) {
        const previewHtml = await server.citationHTML(ui.context.getDocumentPath(), JSON.stringify([source]), csl || null);
        const finalHtml = ensureSafeLinkIsPresent(previewHtml, () => {
          const url = bibMgr.urlForSource(source);
          if (url) {
            return {
              text: ui.context.translateText("[Link]"),
              url
            };
          }
        });

        return (
          <CitePopup
            previewHtml={finalHtml}
            style={style} />
        );
      }
      return null;
    },
    specKey: (target: TextPopupTarget) => {
      return `cite:${target.text}`;
    }
  });
}

function ensureSafeLinkIsPresent(html: string, getLinkData: () => { text: string, url: string } | undefined) {
  const parser = new window.DOMParser();
  const doc = parser.parseFromString(html, 'text/html');

  const linkElements = doc.body.getElementsByTagName('a');
  if (linkElements.length === 0) {

    const linkData = getLinkData();

    // There aren't any links, we should append one 
    // (If links are present, we assume that we shouldn't add another)  
    const paragraphs = doc.body.getElementsByTagName('p');
    if (paragraphs.length === 1 && linkData) {

      // The paragraph containing the formatted source
      const paragraph = paragraphs[0];

      // Create a link to append
      const linkElement = doc.createElement('a');
      linkElement.innerText = linkData.text;
      linkElement.setAttribute('href', linkData.url);
      setLinkTarget(linkElement);

      // Append the link to the formatted source
      paragraph.innerText = paragraph.innerText + ' ';
      paragraph.appendChild(linkElement);
    }
  } else {

    // There are links, ensure all of them have appropriate target information
    // tslint:disable-next-line: prefer-for-of
    for (let i = 0; i < linkElements.length; i++) {
      setLinkTarget(linkElements[i]);
    }
  }
  return doc.body.outerHTML.replace(/\r?\n|\r/g, '');
}

function setLinkTarget(linkElement: HTMLAnchorElement) {
  linkElement.setAttribute('target', '_blank');
  linkElement.setAttribute('rel', 'noopener noreferrer');
}

interface CitePopupProps extends WidgetProps {
  previewHtml: string;
}

const CitePopup: React.FC<CitePopupProps> = props => {
  return (
    <Popup classes={['pm-cite-popup']} style={props.style}>
      <div className='pm-cite-popup-preview'>
        <div dangerouslySetInnerHTML={{ __html: props.previewHtml || '' }} />
      </div>
    </Popup>
  );
};