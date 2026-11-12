// The book's theme: VitePress's default, with the Rust Book's plainness (book.css) and, on each page, a link to the
// page's own Markdown for a model to read (Decision 0011 §5).
import { h } from "vue";
import { useData, withBase } from "vitepress";
import DefaultTheme from "vitepress/theme";
import "./book.css";

const MarkdownLink = {
  setup() {
    const { page } = useData();
    return () => {
      const file = page.value.relativePath.replace(/\.md$/, "");
      return h("p", { class: "book-markdown-link" }, [
        h("a", { href: withBase(`/${file}.md`), target: "_blank" }, "This page as Markdown"),
        " · ",
        h("a", { href: withBase("/llms-full.txt"), target: "_blank" }, "The whole book as Markdown"),
      ]);
    };
  },
};

export default {
  extends: DefaultTheme,
  Layout: () => h(DefaultTheme.Layout, null, { "doc-footer-before": () => h(MarkdownLink) }),
};
